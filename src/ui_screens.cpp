// ui_screens.cpp — the production UI.
// See ui_screens.h for the threading contract and the screen flow.
//
// Layout is designed for 480x272 exactly, no scrolling: this is a fixed panel,
// not a phone. Every screen is a plain lv_obj child of lv_scr_act() that is
// shown/hidden, rather than a separate LVGL screen, so the tree is built once at
// boot and never allocated/freed during operation (no heap churn, no
// use-after-free on a transition mid-render).
//
// TWO RULES THAT CAME FROM REAL FAILURES, and which any change here must keep:
//
//  1. NEVER write an unchanged string to a label. lv_label_set_text() marks the
//     widget dirty unconditionally, so doing it from the main loop is a
//     permanent redraw storm that starves touch input and glitches the panel.
//     Every setter below compares first.
//
//  2. A screen change MUST call touch::forceRelease() before mutating the widget
//     tree. LVGL's indev holds a pointer to the widget it is pressing; if the
//     tree changes underneath it, the next RELEASED is delivered to a hidden
//     widget and the new screen receives no input at all. ui::show() does this.
#include "ui_screens.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>   // M_PI, cosf, sinf for the clock hands
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "net.h"
#include "qr_render.h"
#include "touch_driver.h"

// Implemented in main.cpp. Declared at file scope (NOT inside the anonymous
// namespace below, which would give them internal linkage and conflict with the
// external definitions).
void uiRequestConnect(NetChoice choice);
void uiRequestBack();

namespace ui {

namespace {

constexpr lv_coord_t kW = LCD_WIDTH;
constexpr lv_coord_t kH = LCD_HEIGHT;

// Palette: dark slate background, cyan accents.
constexpr uint32_t kBg      = 0x101018;
constexpr uint32_t kPanel   = 0x1B1B28;
constexpr uint32_t kAccent  = 0x00C8FF;
constexpr uint32_t kGood    = 0x35D07F;
constexpr uint32_t kWarn    = 0xE0A030;
constexpr uint32_t kBad     = 0xE04A4A;
constexpr uint32_t kText    = 0xE8E8F0;
constexpr uint32_t kMuted   = 0x9090A8;
constexpr uint32_t kDim     = 0x606078;
// Pressed-state background for every button: a clearly distinct lighter tone, so
// a working touchscreen gives obvious feedback on contact.
constexpr uint32_t kBtnPressed = 0xF0F0FF;

// --- Light theme (QRIS / payment page) -------------------------------------
//
// The payment page is shown to a CUSTOMER holding a phone, not to the operator,
// so it uses a light theme: a white page reads as a receipt/payment screen and
// gives the QR maximum contrast in a bright room. The dark theme above stays for
// the operator-facing screens (chooser, connecting, summary, standby).
//
// These are deliberately only used by the QRIS page construction and by the
// per-bar theming in statusBar()/setWifiBar(); nothing else should reach for
// them, so a future global theme change cannot silently bleed light colours into
// the dark screens.
constexpr uint32_t kLightBg       = 0xFFFFFF;   // page background: pure white
constexpr uint32_t kLightText     = 0x101018;   // primary text (near-black)
constexpr uint32_t kLightMuted    = 0x6A6A78;   // secondary text
constexpr uint32_t kLightGood     = 0x118A4A;   // total price: dark green on white
constexpr uint32_t kLightBad      = 0xC02020;   // error text: dark red on white
constexpr uint32_t kLightBarBg    = 0xF2F2F6;   // status bar: very light grey
constexpr uint32_t kLightBarRule  = 0xD5D5DE;   // bar divider
constexpr uint32_t kLightBarFg    = 0x30303C;   // bar text

constexpr lv_coord_t kBarH = UI_STATUS_BAR_H;

// QRIS page geometry. Namespace scope because showQris() needs the canvas size
// to report an encode shortfall against the real size. The canvas is FIXED:
// lv_qrcode allocates its buffer in the constructor and cannot be resized.
//
// The page heading ("Pembayaran QRIS") lives in the STATUS BAR, not the body, so
// the QR and the item list start right under the bar and the body is used
// entirely for content. The QR is the right-hand column; the item list is the
// left. The total price sits BELOW the QR, so the QR+total stack must fit the
// panel height (272) under the bar (26).
//
// kQrY is deliberately ABOVE the bar's bottom edge: the top kQrQuiet pixels of
// the QR object are blank white quiet zone, so tucking a few pixels of it under
// the bar loses nothing visible and lifts the code (and everything below it)
// higher on the panel.
constexpr int kQrCanvas = 176;
constexpr int kQrQuiet  = 10;
constexpr int kQrSize   = kQrCanvas + 2 * kQrQuiet;   // on-screen footprint, 196
constexpr int kQrX      = kW - kQrSize - 12;          // right column x
constexpr int kQrY      = kBarH - 6;                  // lift the QR into the bar's quiet zone

// The total sits under the QR, nudged right of the QR column's left edge so it
// reads as a right-hand block rather than aligned with the item list.
constexpr int kTotalXShift = 20;

// Left column layout. No indent: the list is the page's primary content now that
// the heading lives in the status bar.
constexpr int kListX     = 24;                 // left margin of the item list
constexpr int kListRight = 8;                  // gap between list and QR column
constexpr int kListW     = kQrX - kListX - kListRight;

Screen s_current  = Screen::BOOT_SCREEN;
bool   s_latched  = false;   // route committed; WIFI_SELECT is unreachable

lv_obj_t *s_scr = nullptr;

// --- status bar (shared design, ONE SET OF WIDGETS PER PAGE) ---------------
//
// The bar is drawn on every page except the boot log, so there are N copies of
// its three widgets. They are all kept, and setWifiBar() updates every copy.
//
// WHY THIS IS A LIST AND NOT THREE POINTERS: an earlier version kept a single
// {icon, slash, ssid} struct that statusBar() overwrote on each call. Since
// statusBar() runs once per page, only the LAST page built ever received
// updates — every other screen's bar sat frozen on its initial value. That is
// why the Wi-Fi indicator appeared dead on the screen the operator was actually
// looking at.
constexpr uint8_t kMaxBars = 6;
struct StatusBarRefs {
    lv_obj_t *icon  = nullptr;
    lv_obj_t *slash = nullptr;
    lv_obj_t *ssid  = nullptr;
    // True when this bar belongs to a light-themed page (the QRIS page). The
    // colours setWifiBar() pushes depend on it: the "connected" green and the
    // "no wifi" grey that read well on the dark chrome are too pale on a white
    // bar, so each bar carries its own theme rather than sharing one palette.
    bool      light = false;
};
StatusBarRefs s_bars[kMaxBars];
uint8_t       s_barCount = 0;

// Last values pushed into the status bar, so setWifiBar() can skip no-op writes.
bool s_barConnected = false;
char s_barSsid[40]  = {0};

// --- boot screen ---
lv_obj_t *s_boot      = nullptr;
lv_obj_t *s_bootLines = nullptr;   // one label holding the growing log
uint8_t   s_bootShown = 0;
uint32_t  s_bootNext  = 0;

// --- wifi select ---
lv_obj_t *s_select     = nullptr;
lv_obj_t *s_selBanner  = nullptr;

// --- connecting ---
lv_obj_t *s_connect     = nullptr;
lv_obj_t *s_connectSsid = nullptr;
lv_obj_t *s_connectDots = nullptr;
lv_obj_t *s_connectStat = nullptr;

// --- summary ---
lv_obj_t *s_summary   = nullptr;
lv_obj_t *s_sumSsid   = nullptr;
lv_obj_t *s_sumIp     = nullptr;
lv_obj_t *s_sumServer = nullptr;

// --- home ---
lv_obj_t *s_home      = nullptr;
lv_obj_t *s_homeClock = nullptr;   // HH:MM:SS text, the fallback if no NTP
lv_obj_t *s_homeBranch= nullptr;
lv_obj_t *s_homeSync  = nullptr;   // "unsynced" cue
lv_obj_t *s_clockFace = nullptr;
lv_obj_t *s_handH     = nullptr;
lv_obj_t *s_handM     = nullptr;
lv_obj_t *s_handS     = nullptr;

// --- qris ---
lv_obj_t *s_qris    = nullptr;
lv_obj_t *s_qrObj   = nullptr;
lv_obj_t *s_qrTotal = nullptr;        // the price value, below the QR
// The item list is a fixed pool of rows, built once at boot and filled per
// payload. Each row is two labels (price line + name line), so an item never
// needs a widget allocated at runtime — that would be heap churn during a
// payment, against the "build once" rule this file follows.
//
// The count is bounded by the vertical space between the bar and the total
// price. Rows are 34 px tall and start at kBarH + 6 (32); the total sits at
// kQrY + kQrSize + 2 (= 218), so five rows end at 202 and leave a clean gap.
// A sixth would run its name line into the total, so five is the ceiling.
constexpr uint8_t kItemRows = 5;
struct ItemRow {
    lv_obj_t *qtyPrice = nullptr;   // line 1: "2x   Rp 5.000"
    lv_obj_t *name     = nullptr;   // line 2: "ujang - Paket Bocah"
};
ItemRow s_itemRows[kItemRows];

// --- helpers ---------------------------------------------------------------

lv_obj_t *blankPage(lv_obj_t *parent, uint32_t bg = kBg) {
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, kW, kH);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    // A page is a BACKGROUND, not a target. lv_obj_create() is clickable by
    // default, so without this the full-screen container sits in the input chain
    // and can absorb taps meant for whatever is beneath it.
    lv_obj_clear_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                uint32_t color, const lv_font_t *font = nullptr) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (font != nullptr) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// Format a whole-rupiah amount with dot thousands separators into `out`, e.g.
// 5000 -> "5.000". Indonesian convention uses "." as the thousands separator,
// matching how prices read on the panel (the app's own total uses ",").
//
// Writes at most `n` bytes including the terminator; a too-small buffer simply
// truncates. No allocation: this runs inside showQris(), which must not churn the
// heap during a payment.
void formatThousands(int value, char *out, size_t n) {
    if (out == nullptr || n == 0) return;

    // Work on the absolute value, then re-apply the sign (prices are positive,
    // but this stays correct if a negative ever arrives).
    const bool neg = value < 0;
    unsigned int v = neg ? (unsigned int)(-value) : (unsigned int)value;

    char digits[16];
    snprintf(digits, sizeof(digits), "%u", v);

    size_t o = 0;
    if (neg && o < n - 1) out[o++] = '-';
    const int len = (int)strlen(digits);
    for (int i = 0; i < len && o < n - 1; i++) {
        if (i > 0 && ((len - i) % 3) == 0 && o < n - 1) out[o++] = '.';
        if (o < n - 1) out[o++] = digits[i];
    }
    out[o] = '\0';
}

// Show a QR failure message in place of the total, in red. Used when the payload
// cannot be encoded at all — the total is meaningless without a QR to pay into,
// so it is replaced rather than left showing a number the customer cannot use.
void showQrError(const char *text) {
    if (s_qrTotal == nullptr || text == nullptr) return;
    lv_label_set_text(s_qrTotal, text);
    lv_obj_set_style_text_color(s_qrTotal, lv_color_hex(kLightBad), 0);
}

// Action sentinels that cannot collide with NetChoice (which is 0..3).
constexpr int kActContinue = 100;
constexpr int kActDebug    = 101;
constexpr int kActCancel   = 102;

void onSelectButton(lv_event_t *e) {
    const int action = (int)(intptr_t)lv_event_get_user_data(e);
    if (action == kActDebug) {
        Serial.println("[ui] wifi select -> Debug (IDRP)");
        uiRequestConnect(NetChoice::DEBUG);
        return;
    }
    // NetChoice values are 1..3 (the two visible APs are WARNET and CAFE).
    const NetChoice choice = (NetChoice)action;
    Serial.printf("[ui] wifi select -> \"%s\"\n", net::ssidFor(choice));
    uiRequestConnect(choice);
}

void onContinue(lv_event_t *e) {
    (void)e;
    // The route latch. From here on there is no path back to WIFI_SELECT for the
    // life of this boot, which is the deliberate product decision: the operator
    // commits to a branch when they install the device.
    s_latched = true;
    Serial.println("[ui] summary -> Continue (route LATCHED, wifi select locked out)");
    showHome();
}

void onConnectingCancel(lv_event_t *e) {
    (void)e;
    Serial.println("[ui] connecting -> Cancel (aborting association)");
    uiRequestBack();
}

// Long-press on the Debug button is deliberately NOT used: a plain small button
// is simpler and does not depend on LVGL long-press timing interacting with the
// touch driver's stale-press watchdog.

lv_obj_t *makeButton(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                     lv_coord_t w, lv_coord_t h, uint32_t bg, int action,
                     lv_event_cb_t cb, const lv_font_t *font = nullptr) {
    lv_obj_t *b = lv_btn_create(parent);

    // Do NOT use lv_obj_remove_style_all() here. It also strips the theme's
    // LV_STATE_PRESSED styling, which means tapping a button produces no visual
    // feedback at all — on a device whose headline symptom was "touch does not
    // work", an unresponsive-looking button is actively misleading. Keep the
    // theme default and only recolour the background.
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(b, lv_color_hex(kBtnPressed), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)action);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, font ? font : &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return b;
}

void hideAll() {
    lv_obj_t *pages[] = {s_boot,   s_select, s_connect,
                         s_summary, s_home,  s_qris};
    for (lv_obj_t *p : pages) {
        if (p) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    }
}

// Build the shared top status bar into `page`. Called for every page except the
// boot log, which owns the whole screen.
//
// `light` selects the light chrome used by the payment (QRIS) page; every other
// page keeps the dark chrome. The flag is also recorded per bar so setWifiBar()
// can pick compatible foreground colours later (the bar has no text to read back
// once built).
//
// `rightText`, when non-null, is drawn right-aligned in the same bar row (used
// by the QRIS page for its "Pembayaran QRIS" heading, which lives here rather than in
// the page body so the whole body is free for the item list).
void statusBar(lv_obj_t *page, bool light = false, const char *rightText = nullptr) {
    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kW, kBarH);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(light ? kLightBarBg : UI_STATUS_BAR_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    // Decorative strip, not a target: it spans the full width and would
    // otherwise sit in the input chain over the top of the page.
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    // Small divider so the bar reads as chrome rather than part of the content.
    lv_obj_t *rule = lv_obj_create(bar);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, kW, 1);
    lv_obj_set_pos(rule, 0, kBarH - 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(light ? kLightBarRule : 0x2A2A3C), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);

    // Wi-Fi glyph. Montserrat has WIFI but no "wifi-off", so the disconnected
    // state is this glyph plus a slash drawn over it (see the slash below).
    //
    // The glyph is 20 px tall, so its box is roughly 0..30 horizontally when
    // left-aligned at x=8. The SSID starts at x=46, leaving ~16 px of padding
    // between the glyph and the text so they do not read as one blob.
    lv_obj_t *icon = lv_label_create(bar);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(icon, lv_color_hex(kMuted), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 8, 0);

    // The slash. A single lv_line segment across the glyph, hidden while the
    // link is up. Points are absolute within the bar, so they track the icon's
    // new position.
    static lv_point_t slashPts[2];
    slashPts[0].x = 10;
    slashPts[0].y = kBarH - 6;
    slashPts[1].x = 26;
    slashPts[1].y = 4;
    lv_obj_t *slash = lv_line_create(bar);
    lv_line_set_points(slash, slashPts, 2);
    lv_obj_set_style_line_width(slash, 2, 0);
    lv_obj_set_style_line_color(slash, lv_color_hex(light ? kLightBad : kBad), 0);
    lv_obj_clear_flag(slash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(slash, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ssid = lv_label_create(bar);
    lv_label_set_text(ssid, "no wifi");
    lv_obj_set_style_text_color(ssid, lv_color_hex(light ? kLightBarFg : UI_STATUS_BAR_FG), 0);
    lv_obj_set_style_text_font(ssid, &lv_font_montserrat_14, 0);
    lv_obj_align(ssid, LV_ALIGN_LEFT_MID, 46, 0);

    // Optional right-aligned heading in the same bar row. On the QRIS page this
    // is "Pembayaran QRIS": putting it here instead of the page body frees the whole
    // body area for the item list, which is the tightest resource on the page.
    if (rightText != nullptr) {
        lv_obj_t *right = lv_label_create(bar);
        lv_label_set_text(right, rightText);
        lv_obj_set_style_text_color(right,
                                    lv_color_hex(light ? kLightBarFg : UI_STATUS_BAR_FG), 0);
        // 16 px so it reads as a heading, but still fits within the 26 px bar.
        lv_obj_set_style_text_font(right, &lv_font_montserrat_16, 0);
        lv_obj_align(right, LV_ALIGN_RIGHT_MID, -12, 0);
    }

    // Register this page's copy so setWifiBar() updates ALL of them, not just
    // the last one built. See the StatusBarRefs comment for why this matters.
    if (s_barCount < kMaxBars) {
        s_bars[s_barCount].icon  = icon;
        s_bars[s_barCount].slash = slash;
        s_bars[s_barCount].ssid  = ssid;
        s_bars[s_barCount].light = light;
        s_barCount++;
    } else {
        // Should not happen (5 pages, 6 slots), but say so rather than silently
        // dropping a bar out of the update loop.
        Serial.println("[ui] WARNING: status bar registry full; bar will not update");
    }
}

// --- boot log --------------------------------------------------------------
//
// Purely cosmetic, and deliberately so: every entry below reflects something
// that has ALREADY happened by the time this screen is shown, because main.cpp
// does all real initialisation before calling showBootScreen(). It can therefore
// never hide an init failure — if init fails we never reach this screen at all.
//
// The lines are revealed a few at a time from tick(). They are static text
// rather than live status, which is honest: this is a boot *animation*.
const char *const kBootLog[] = {
    "indorep_payment_pos v1.0",
    " ",
    "[ ok ] cpu      esp32-s3 @ 240 mhz",
    "[ ok ] psram    8192 kb opi",
    "[ ok ] flash    16384 kb",
    "[ ok ] display  ili6485 480x272 rgb",
    "[ ok ] touch    gt911 @0x5d",
    "[ ok ] lvgl     8.4.0",
    "[ ok ] web      http server on :80",
    " ",
    "starting payment terminal...",
};
constexpr uint8_t kBootLogCount = sizeof(kBootLog) / sizeof(kBootLog[0]);

void tickBoot() {
    if (s_bootLines == nullptr) return;

    // Advance roughly every 160 ms. The first line appears immediately so the
    // screen is never blank.
    if (s_bootShown > 0 && millis() < s_bootNext) return;
    s_bootNext = millis() + 160;

    if (s_bootShown >= kBootLogCount) {
        // Log finished: hand over to the Wi-Fi chooser.
        showWifiSelect();
        return;
    }

    // Rebuild the whole label text. It is one label, rewritten once per line
    // during boot (~11 times) and never again, so this is nothing like the
    // hot-path storm the setters below guard against.
    //
    // Loop bound is `< kBootLogCount` guarded by `<= s_bootShown` so the count
    // and the array can never disagree even if a line is added later.
    static char buf[512];
    const uint8_t upto = (s_bootShown < kBootLogCount) ? s_bootShown
                                                       : (uint8_t)(kBootLogCount - 1);
    int n = 0;
    for (uint8_t i = 0; i <= upto && n < (int)sizeof(buf) - 64; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s\n", kBootLog[i]);
    }
    s_bootShown++;
    lv_label_set_text(s_bootLines, buf);
}

// --- connecting dots -------------------------------------------------------
void tickConnectingDots() {
    if (s_connectDots == nullptr) return;

    static uint32_t last = 0;
    if (millis() - last < 330) return;
    last = millis();

    static uint8_t phase = 0;
    phase = (uint8_t)((phase + 1) % 4);
    static const char *frames[] = {".", "..", "...", "...."};
    const char *want = frames[phase];

    const char *cur = lv_label_get_text(s_connectDots);
    if (cur != nullptr && strcmp(cur, want) == 0) return;
    lv_label_set_text(s_connectDots, want);
}

// Compute the two endpoints of one hand, as a fraction `len` of the radius.
//
// `value`/`steps` is the hand's position as a fraction of a full turn, so all
// three hands share this helper: seconds and minutes use steps=60 directly, the
// hour hand uses steps=60 with value=(hour%12)*5 + minute/12.
//
// The -PI/2 offset puts value 0 at 12 o'clock rather than 3 o'clock, and because
// LVGL's y axis grows DOWNWARD, sin() correctly yields "up" as negative y. So:
//   value 0  -> straight up      (12 o'clock)
//   value 15 -> straight right   (3 o'clock)
void clockHandPoints(uint8_t value, uint8_t steps, float len, lv_point_t pts[2]) {
    const float radius = UI_CLOCK_DIAMETER / 2.0f - 4.0f;
    const float angle = (2.0f * (float)M_PI * (float)value) / (float)steps
                        - (float)M_PI / 2.0f;
    const lv_coord_t cx = UI_CLOCK_DIAMETER / 2;
    const lv_coord_t cy = UI_CLOCK_DIAMETER / 2;
    pts[0].x = cx;
    pts[0].y = cy;
    pts[1].x = (lv_coord_t)((float)cx + cosf(angle) * radius * len);
    pts[1].y = (lv_coord_t)((float)cy + sinf(angle) * radius * len);
}

void updateClock() {
    if (s_handH == nullptr) return;

    const net::TimeOfDay t = net::localTime();
    const bool ready = net::timeReady();

    // Redraw only when something the face depends on changed. Note the digital
    // readout shows SECONDS, so a change of second is a real change; hour/minute
    // are checked too so a time jump (a late sync) is never missed.
    //
    // `lastReady` is the important one: the transition false -> true is what
    // clears the "belum sinkron" cue, and it must not be missed even if the
    // hour/minute/second happen to be unchanged.
    static uint8_t lastS = 0xFF, lastM = 0xFF, lastH = 0xFF;
    static int lastReady = -1;
    const int readyNow = ready ? 1 : 0;

    const bool changed = (readyNow != lastReady) || (t.second != lastS) ||
                         (t.minute != lastM) || (t.hour != lastH);
    if (!changed) return;

    // Log the sync transition once. This is the point the "belum sinkron" cue is
    // cleared, so if the screen and this log ever disagree, the log dates it.
    if (readyNow != lastReady) {
        Serial.printf("[ui] clock %s (state %d -> %d)\n",
                      ready ? "SYNCED, clearing cue" : "unsynced",
                      lastReady, readyNow);
    }

    lastS = t.second;
    lastM = t.minute;
    lastH = t.hour;
    lastReady = readyNow;

    // Hands in 60-step units so one helper covers all three:
    //   second -> 0..59 steps (1 step = 6 degrees)
    //   minute -> 0..59
    //   hour   -> (hour%12)*5 + minute/12, i.e. it advances within the hour
    //             instead of jumping, which is what makes it read as a real clock
    const uint8_t hourSteps = (uint8_t)((t.hour % 12) * 5 + t.minute / 12);
    const uint8_t minSteps  = t.minute;

    static lv_point_t ptsH[2], ptsM[2], ptsS[2];
    clockHandPoints(hourSteps, 60, 0.52f, ptsH);
    clockHandPoints(minSteps,  60, 0.76f, ptsM);
    clockHandPoints(t.second,  60, 0.86f, ptsS);
    lv_line_set_points(s_handH, ptsH, 2);
    lv_line_set_points(s_handM, ptsM, 2);
    lv_line_set_points(s_handS, ptsS, 2);

    // Digital readout doubles as the "unsynced" indicator: showing a dial that
    // silently reads 12:00 forever would look like a frozen screen, so when the
    // clock has not synced we say so plainly and show --:-- instead of a
    // plausible-looking wrong time.
    //
    // The sync cue is set from `ready` on EVERY redraw (not only on the
    // transition), so it can never be left visible by a missed edge.
    char buf[32];
    if (ready) {
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u", t.hour, t.minute, t.second);
        lv_obj_add_flag(s_homeSync, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(buf, sizeof(buf), "--:--:--");
        lv_obj_clear_flag(s_homeSync, LV_OBJ_FLAG_HIDDEN);
    }
    const char *cur = lv_label_get_text(s_homeClock);
    if (cur == nullptr || strcmp(cur, buf) != 0) {
        lv_label_set_text(s_homeClock, buf);
    }

    // Dim the hands while unsynced, so the face reads as "not live" at a glance.
    const uint32_t handCol = ready ? kText : kDim;
    lv_obj_set_style_line_color(s_handH, lv_color_hex(handCol), 0);
    lv_obj_set_style_line_color(s_handM, lv_color_hex(handCol), 0);
    lv_obj_set_style_line_color(s_handS, lv_color_hex(ready ? kAccent : kDim), 0);
}

void show(lv_obj_t *page, Screen which) {
    // Order matters, and this ordering is the fix for "after navigating, touch
    // stops working":
    //
    //  1. Release the touch press FIRST, before any widget changes. LVGL's indev
    //     tracks the object it is currently pressing. If the widget tree changes
    //     while that pointer is set, the RELEASED event goes to a widget on the
    //     screen we just hid, the indev's press never completes, and the new
    //     screen receives no input whatsoever.
    touch::forceRelease();

    hideAll();
    if (page) {
        lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(page);
    }
    s_current = which;
    Serial.printf("[ui] screen -> %d\n", (int)which);
}

}  // namespace

// ---------------------------------------------------------------------------
void build() {
    s_scr = lv_scr_act();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(kBg), 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---------------- BOOT SCREEN ----------------
    // Terminal-style log. No status bar here: this screen owns the whole panel.
    s_boot = blankPage(s_scr);
    lv_obj_t *bootInner = lv_obj_create(s_boot);
    lv_obj_remove_style_all(bootInner);
    lv_obj_set_size(bootInner, kW - 40, kH - 40);
    lv_obj_set_pos(bootInner, 20, 20);
    lv_obj_set_style_bg_color(bootInner, lv_color_hex(0x0A0A10), 0);
    lv_obj_set_style_bg_opa(bootInner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bootInner, 1, 0);
    lv_obj_set_style_border_color(bootInner, lv_color_hex(0x2A3A2A), 0);
    lv_obj_set_style_radius(bootInner, 6, 0);
    lv_obj_clear_flag(bootInner, LV_OBJ_FLAG_CLICKABLE);

    s_bootLines = lv_label_create(bootInner);
    lv_label_set_text(s_bootLines, "");
    lv_obj_set_style_text_color(s_bootLines, lv_color_hex(0x7CE38B), 0);
    lv_obj_set_style_text_font(s_bootLines, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_bootLines, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_bootLines, kW - 72);
    lv_obj_set_pos(s_bootLines, 12, 8);

    // ---------------- WIFI SELECT ----------------
    s_select = blankPage(s_scr);
    statusBar(s_select);
    label(s_select, "Pilih jaringan", 24, kBarH + 16, kText, &lv_font_montserrat_20);

    makeButton(s_select, "WARNET", 24, kBarH + 54, 200, 76, 0x2E7D32,
               (int)NetChoice::WARNET, onSelectButton);
    makeButton(s_select, "CAFE", 256, kBarH + 54, 200, 76, 0x1565C0,
               (int)NetChoice::CAFE, onSelectButton);

    // Failure banner, empty unless an attempt failed.
    s_selBanner = label(s_select, "", 24, kBarH + 142, kBad);
    lv_obj_set_width(s_selBanner, kW - 48);
    lv_label_set_long_mode(s_selBanner, LV_LABEL_LONG_WRAP);

    // Small, subordinate Debug entry point bottom-right: clickable, but clearly
    // not part of the normal choice.
    makeButton(s_select, "Debug", kW - 24 - 92, kH - 14 - 30, 92, 30,
               0x3A3A4C, kActDebug, onSelectButton, &lv_font_montserrat_14);

    // ---------------- CONNECTING ----------------
    s_connect = blankPage(s_scr);
    statusBar(s_connect);
    label(s_connect, "Menghubungkan ke", 24, kBarH + 28, kMuted);
    s_connectSsid = label(s_connect, "-", 24, kBarH + 52, kText,
                          &lv_font_montserrat_20);
    s_connectDots = label(s_connect, "...", 24, kBarH + 84, kText,
                          &lv_font_montserrat_20);
    s_connectStat = label(s_connect, "memulai...", 24, kBarH + 114, kMuted);
    lv_obj_set_width(s_connectStat, 300);
    lv_label_set_long_mode(s_connectStat, LV_LABEL_LONG_WRAP);

    makeButton(s_connect, "Batal", 24, kH - 14 - 44, 160, 44, 0x9A4A4A,
               kActCancel, onConnectingCancel);

    // ---------------- SUMMARY ----------------
    s_summary = blankPage(s_scr);
    statusBar(s_summary);

    // Big green check, centred in the free area below the bar.
    lv_obj_t *check = lv_label_create(s_summary);
    lv_label_set_text(check, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(check, lv_color_hex(kGood), 0);
    lv_obj_set_style_text_font(check, &lv_font_montserrat_32, 0);
    lv_obj_align(check, LV_ALIGN_TOP_MID, 0, kBarH + 14);

    // "Terhubung!" is centred explicitly rather than by child index: relying on
    // "the last child is the label I just made" breaks silently the moment
    // anything else is added to this page.
    lv_obj_t *titleLbl = label(s_summary, "Terhubung!", 0, 0, kText,
                               &lv_font_montserrat_20);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, kBarH + 58);

    s_sumSsid = label(s_summary, "-", 0, 0, kGood, &lv_font_montserrat_16);
    lv_obj_align(s_sumSsid, LV_ALIGN_TOP_MID, 0, kBarH + 88);

    // The DHCP-assigned address, shown so the operator (and the backend) can read
    // the device's live IP straight off the panel. This is the number to put in a
    // router reservation or to point the backend at, so it gets its own line.
    s_sumIp = label(s_summary, "IP -", 0, 0, kText, &lv_font_montserrat_16);
    lv_obj_align(s_sumIp, LV_ALIGN_TOP_MID, 0, kBarH + 116);

    s_sumServer = label(s_summary, "-", 0, 0, kMuted, &lv_font_montserrat_14);
    lv_obj_align(s_sumServer, LV_ALIGN_TOP_MID, 0, kBarH + 142);

    // Large Continue. Pressing it latches the route permanently.
    makeButton(s_summary, "Lanjut", kW / 2 - 110, kH - 20 - 56, 220, 56,
               0x2E7D32, kActContinue, onContinue, &lv_font_montserrat_20);

    // ---------------- HOME (standby) ----------------
    s_home = blankPage(s_scr);
    statusBar(s_home);

    // Clock face: a bordered circle. The hands are lv_line objects whose points
    // are recomputed each second inside updateClock().
    s_clockFace = lv_obj_create(s_home);
    lv_obj_remove_style_all(s_clockFace);
    lv_obj_set_size(s_clockFace, UI_CLOCK_DIAMETER, UI_CLOCK_DIAMETER);
    lv_obj_set_pos(s_clockFace, UI_CLOCK_X, UI_CLOCK_Y);
    lv_obj_set_style_radius(s_clockFace, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_clockFace, 2, 0);
    lv_obj_set_style_border_color(s_clockFace, lv_color_hex(0x2F3F55), 0);
    lv_obj_set_style_bg_color(s_clockFace, lv_color_hex(0x14141F), 0);
    lv_obj_set_style_bg_opa(s_clockFace, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_clockFace, LV_OBJ_FLAG_CLICKABLE);

    s_handH = lv_line_create(s_clockFace);
    s_handM = lv_line_create(s_clockFace);
    s_handS = lv_line_create(s_clockFace);
    lv_obj_set_style_line_width(s_handH, 4, 0);
    lv_obj_set_style_line_width(s_handM, 3, 0);
    lv_obj_set_style_line_width(s_handS, 1, 0);
    lv_obj_set_style_line_color(s_handH, lv_color_hex(kText), 0);
    lv_obj_set_style_line_color(s_handM, lv_color_hex(kText), 0);
    lv_obj_set_style_line_color(s_handS, lv_color_hex(kAccent), 0);
    lv_obj_set_style_line_rounded(s_handH, true, 0);
    lv_obj_set_style_line_rounded(s_handM, true, 0);
    for (lv_obj_t *h : {s_handH, s_handM, s_handS}) {
        lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    }

    // Digital readout under the face, plus the unsynced cue.
    //
    // LEFT-ALIGNED with the clock face (as before), not centred: centring pushed
    // the text right to under the "Indorep" branch label, which read as two
    // competing blocks. A small left inset (kClockTextPadX) nudges it off the
    // face's exact left edge so it does not look glued to it.
    constexpr lv_coord_t kClockTextPadX = 12;

    s_homeClock = label(s_home, "--:--:--", UI_CLOCK_X + kClockTextPadX,
                        UI_CLOCK_Y + UI_CLOCK_DIAMETER + 6, kMuted,
                        &lv_font_montserrat_20);

    s_homeSync = label(s_home, "belum sinkron", UI_CLOCK_X + kClockTextPadX,
                       UI_CLOCK_Y + UI_CLOCK_DIAMETER + 32, kWarn,
                       &lv_font_montserrat_12);
    // Start hidden, shown by updateClock() when there is no time.
    lv_obj_add_flag(s_homeSync, LV_OBJ_FLAG_HIDDEN);

    // Branch name, right-hand side.
    s_homeBranch = label(s_home, "Indorep", 250, kBarH + 90, kText,
                         &lv_font_montserrat_32);

    // ---------------- QRIS ----------------
    // LIGHT THEME. Unlike every other screen this one faces the CUSTOMER, so it
    // is a white receipt-style page: maximum QR contrast in a bright counter
    // environment, and it reads as a payment screen rather than device chrome.
    // The background is white and every text element below is recoloured for a
    // white page; the shared status bar is built in its light variant, and it
    // carries the "Pembayaran QRIS" heading so the body is free for the item list.
    s_qris = blankPage(s_scr, kLightBg);
    statusBar(s_qris, true, "Pembayaran QRIS");

    // The CANVAS size is passed, not the on-screen footprint: qr::create() adds
    // the quiet-zone padding itself. Passing kQrSize here would double it.
    s_qrObj = qr::create(s_qris, kQrX + kQrQuiet, kQrY + kQrQuiet, kQrCanvas);

    // Item rows: a fixed pool, two lines each. The first line carries the
    // quantity and unit price, the second the name/member. All built hidden;
    // showQris() reveals the ones a payload has and fills them.
    //
    // There is no divider between rows: it cost vertical pitch for little gain,
    // and on a light page the two-line blocks already read as separate entries.
    //
    // The list starts BELOW the status bar (kBarH), not at kQrY: the QR is
    // allowed to tuck into the bar's area because its top is blank quiet zone,
    // but text must not overlap the bar. So the list gets its own top padding.
    constexpr lv_coord_t kRowH    = 34;   // vertical pitch of one item
    constexpr lv_coord_t kListTop = kBarH + 6;  // first row's top, under the bar
    constexpr lv_coord_t kLine1Y  = 0;    // qty/price line offset within a row
    constexpr lv_coord_t kLine2Y  = 16;   // name line offset within a row
    for (uint8_t i = 0; i < kItemRows; i++) {
        const lv_coord_t rowY = kListTop + (lv_coord_t)i * kRowH;
        ItemRow &r = s_itemRows[i];

        // Line 1: quantity and unit price, the heavier of the two lines.
        r.qtyPrice = label(s_qris, "", kListX, rowY + kLine1Y, kLightText,
                           &lv_font_montserrat_14);
        lv_obj_set_width(r.qtyPrice, kListW);
        lv_label_set_long_mode(r.qtyPrice, LV_LABEL_LONG_CLIP);

        // Line 2: name (and member for billing lines), muted so the price line
        // stays the visual anchor.
        r.name = label(s_qris, "", kListX, rowY + kLine2Y, kLightMuted,
                       &lv_font_montserrat_12);
        lv_obj_set_width(r.name, kListW);
        lv_label_set_long_mode(r.name, LV_LABEL_LONG_DOT);

        // Start hidden; showQris() decides what to show per payload.
        lv_obj_add_flag(r.qtyPrice, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r.name, LV_OBJ_FLAG_HIDDEN);
    }

    // Total value, BELOW the QR code. No "Total:" caption: the size and the
    // green colour make it self-evident, and the freed line keeps the item list
    // room. It is the largest text on the page because it is the number the
    // customer checks before paying.
    s_qrTotal = lv_label_create(s_qris);
    lv_label_set_text(s_qrTotal, "");
    lv_obj_set_style_text_color(s_qrTotal, lv_color_hex(kLightGood), 0);
    lv_obj_set_style_text_font(s_qrTotal, &lv_font_montserrat_32, 0);
    // Under the QR column, nudged right so it reads as a right-hand block.
    lv_obj_set_pos(s_qrTotal, kQrX + kTotalXShift, kQrY + kQrSize + 2);

    showBootScreen();
}

// ---------------------------------------------------------------------------
// --- screen transitions (main-loop task only) ------------------------------

void showBootScreen() {
    s_bootShown = 0;
    s_bootNext = millis();
    if (s_bootLines) lv_label_set_text(s_bootLines, "");
    show(s_boot, Screen::BOOT_SCREEN);
}

bool bootScreenActive() { return s_current == Screen::BOOT_SCREEN; }

void showWifiSelect() {
    // The latch is enforced HERE as well as in show(), because this is the one
    // function that could otherwise be called by mistake after the route is
    // committed (e.g. from a stray tick()).
    if (s_latched) {
        Serial.println("[ui] showWifiSelect() refused: route is latched");
        return;
    }
    show(s_select, Screen::WIFI_SELECT);
}

void showConnecting(const char *ssid) {
    if (s_connectSsid) lv_label_set_text(s_connectSsid, ssid ? ssid : "-");
    if (s_connectStat) lv_label_set_text(s_connectStat, "memulai...");
    if (s_selBanner) lv_label_set_text(s_selBanner, "");
    show(s_connect, Screen::CONNECTING);
}

void showConnectFailed(const char *ssid, const char *reason) {
    if (s_latched) return;   // cannot happen, but never un-latch by accident
    char buf[192];
    snprintf(buf, sizeof(buf), "Gagal terhubung ke \"%s\":\n%s",
             ssid ? ssid : "-", reason ? reason : "-");
    if (s_selBanner) lv_label_set_text(s_selBanner, buf);
    Serial.printf("[ui] connection failure banner: %s\n", buf);
    showWifiSelect();
}

void setConnectingStatus(const char *text) {
    if (s_connectStat == nullptr || text == nullptr) return;

    // NEVER write an unchanged string: lv_label_set_text() marks the label dirty
    // unconditionally, so doing it from the main loop is a permanent redraw
    // storm that starves touch input.
    const char *cur = lv_label_get_text(s_connectStat);
    if (cur != nullptr && strcmp(cur, text) == 0) return;

    lv_label_set_text(s_connectStat, text);
}

void showSummary(const char *ssid, const char *ip, bool serverOk) {
    if (s_sumSsid) lv_label_set_text(s_sumSsid, ssid ? ssid : "-");

    // The DHCP-assigned address. This is the headline fact on this screen now
    // that the device no longer claims a fixed IP: it is what the operator reads
    // off the panel to find the device, and what the backend must be pointed at
    // (or reserved for by MAC).
    if (s_sumIp) {
        char buf[48];
        snprintf(buf, sizeof(buf), "IP %s", (ip && ip[0]) ? ip : "-");
        lv_label_set_text(s_sumIp, buf);
    }

    // Server state is the only other technical line on this screen. It is shown
    // because a FAILED HTTP server means the device will never receive a
    // payment, and that is worth one line of screen before the operator commits.
    if (s_sumServer) {
        lv_label_set_text(s_sumServer,
                          serverOk ? "server siap" : "server GAGAL");
        lv_obj_set_style_text_color(s_sumServer,
                                    lv_color_hex(serverOk ? kMuted : kBad), 0);
    }

    show(s_summary, Screen::SUMMARY);
}

void showHome() {
    // Diagnostics (the serial 'r' command, and showQris) reach standby without
    // going through setup, so latch here too rather than leaving the chooser
    // reachable behind a standby screen. The Continue button sets the latch
    // before calling this, so in the normal path this is a no-op.
    if (!s_latched) {
        s_latched = true;
        Serial.println("[ui] route latched implicitly (reached standby without setup)");
    }
    show(s_home, Screen::HOME);
    updateClock();   // paint the face immediately rather than on the next tick
}

void showQris(const PaymentPayload &payload) {
    // Diagnostics reach this without going through setup (the 'q' console
    // command renders a local test payload). Latch the route in that case, so a
    // bench test does not leave the device in a state where a stray tick could
    // bounce it back to the Wi-Fi chooser.
    if (!s_latched) {
        s_latched = true;
        Serial.println("[ui] route latched implicitly by showQris()");
    }

    // --- item rows ---------------------------------------------------------
    //
    // Each row is two lines: "Nx   Rp unit" then the name/member. Rows beyond
    // what the payload carries (or beyond the fixed pool) stay hidden. A cart
    // longer than the pool is truncated with a trailing "+N lagi" line so the
    // operator is never misled into thinking a partial list is complete.
    int row = 0;
    for (int i = 0; i < payload.item_count && row < kItemRows; i++, row++) {
        const PaymentItem &it = payload.items[i];
        ItemRow &r = s_itemRows[row];

        if (r.qtyPrice != nullptr) {
            char line1[40];
            if (it.price > 0) {
                // Thousands-separated rupiah, e.g. "2x   Rp 5.000".
                char price[20];
                formatThousands(it.price, price, sizeof(price));
                snprintf(line1, sizeof(line1), "%dx   Rp %s", it.qty, price);
            } else {
                // No price in the payload (older POS): quantity only, rather
                // than a misleading "Rp 0".
                snprintf(line1, sizeof(line1), "%dx", it.qty);
            }
            lv_label_set_text(r.qtyPrice, line1);
            lv_obj_clear_flag(r.qtyPrice, LV_OBJ_FLAG_HIDDEN);
        }
        if (r.name != nullptr) {
            lv_label_set_text(r.name, it.item[0] ? it.item : "-");
            lv_obj_clear_flag(r.name, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Overflow marker: if there are still items after the pool is full, show
    // "+N lagi" in the name line of the last row so the list is honestly partial.
    if (payload.item_count > row && row > 0) {
        ItemRow &last = s_itemRows[row - 1];
        char more[24];
        snprintf(more, sizeof(more), "+%d lagi", payload.item_count - (row - 1));
        if (last.name != nullptr) lv_label_set_text(last.name, more);
    }

    // Hide any rows the payload did not use, so a shorter cart after a longer
    // one does not leave stale text on screen.
    for (int i = row; i < kItemRows; i++) {
        ItemRow &r = s_itemRows[i];
        if (r.qtyPrice) lv_obj_add_flag(r.qtyPrice, LV_OBJ_FLAG_HIDDEN);
        if (r.name)     lv_obj_add_flag(r.name, LV_OBJ_FLAG_HIDDEN);
    }
    if (payload.item_count == 0) {
        // Empty cart (cash-style summary): say so in the first row rather than
        // showing a blank list.
        ItemRow &r = s_itemRows[0];
        if (r.qtyPrice) lv_obj_add_flag(r.qtyPrice, LV_OBJ_FLAG_HIDDEN);
        if (r.name) {
            lv_label_set_text(r.name, "(tanpa rincian item)");
            lv_obj_clear_flag(r.name, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_qrTotal) {
        lv_label_set_text(s_qrTotal, payload.total[0] ? payload.total : "");
        lv_obj_set_style_text_color(s_qrTotal, lv_color_hex(kLightGood), 0);
    }

    if (payload.has_qr) {
        // The canvas was created at a fixed size (lv_qrcode allocates its buffer
        // in the constructor and cannot be resized), so just re-encode into it.
        //
        // requiredSize() is a pre-flight check: it tells us up front whether the
        // payload can be encoded at all, which makes a "QR failed" report
        // immediately actionable in the log.
        const int wantPx = qr::requiredSize(payload.qr);
        if (wantPx <= 0) {
            Serial.println("[ui] payload exceeds QR capacity");
            qr::setVisible(s_qrObj, false);
            showQrError("QR GAGAL DIBUAT");
        } else if (qr::update(s_qrObj, payload.qr)) {
            qr::setVisible(s_qrObj, true);
        } else {
            Serial.printf("[ui] QR encode failed (wanted %dpx in a %dpx canvas)\n",
                          wantPx, kQrCanvas);
            qr::setVisible(s_qrObj, false);
            showQrError("QR GAGAL DIBUAT");
        }
    } else {
        // Cash-style summary: no QR, just the cart and the total.
        qr::setVisible(s_qrObj, false);
    }

    show(s_qris, Screen::QRIS);
}

void setWifiBar(bool connected, const char *ssid) {
    const char *wantSsid = (ssid && ssid[0]) ? ssid : "no wifi";

    // Change-detected on BOTH fields, because a redundant lv_label_set_text()
    // from the main loop is a redraw storm (see the file header). This also
    // means the loop below only runs when something actually changed, so
    // touching N bars is still cheap.
    const bool ssidChanged = (strcmp(s_barSsid, wantSsid) != 0);
    if (connected == s_barConnected && !ssidChanged) return;

    s_barConnected = connected;
    snprintf(s_barSsid, sizeof(s_barSsid), "%s", wantSsid);

    // Update EVERY page's copy. There is one bar per page, so updating only one
    // would leave the indicator stale on every other screen.
    for (uint8_t i = 0; i < s_barCount; i++) {
        StatusBarRefs &b = s_bars[i];

        if (b.icon) {
            // Per-bar theme: the green/grey pair that reads well on the dark
            // chrome is too pale against the light QRIS bar, so the light bar
            // gets a darker green and a mid grey.
            const uint32_t iconCol = connected
                                         ? (b.light ? kLightGood : kGood)
                                         : (b.light ? kLightMuted : kMuted);
            lv_obj_set_style_text_color(b.icon, lv_color_hex(iconCol), 0);
        }
        if (b.slash) {
            // The slash is the "off" indicator: shown only while disconnected.
            if (connected) {
                lv_obj_add_flag(b.slash, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(b.slash, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (b.ssid && ssidChanged) {
            lv_label_set_text(b.ssid, wantSsid);
        }
    }
}

Screen current() { return s_current; }

bool routeLatched() { return s_latched; }

void tick() {
    if (s_current == Screen::BOOT_SCREEN) {
        tickBoot();
        return;
    }
    if (s_current == Screen::CONNECTING) {
        tickConnectingDots();
        return;
    }
    if (s_current == Screen::HOME) {
        updateClock();
        return;
    }
}

}  // namespace ui
