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

constexpr lv_coord_t kBarH = UI_STATUS_BAR_H;

// QRIS page geometry. Namespace scope because showQris() needs the canvas size
// to report an encode shortfall against the real size. The canvas is FIXED:
// lv_qrcode allocates its buffer in the constructor and cannot be resized.
//
// The canvas is 196 px and qr::create() adds a 10 px white quiet zone on each
// side, so the object occupies 216 px on screen and the black modules are never
// flush with its edge. Ends at y = 44 + 216 = 260, leaving a 12 px margin.
constexpr int kQrCanvas = 196;
constexpr int kQrQuiet  = 10;
constexpr int kQrSize   = kQrCanvas + 2 * kQrQuiet;   // on-screen footprint, 216
constexpr int kQrX      = kW - kQrSize - 12;
constexpr int kQrY      = kBarH + 18;

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
lv_obj_t *s_qrItems = nullptr;
lv_obj_t *s_qrTotal = nullptr;
lv_obj_t *s_qrTitle = nullptr;

// --- helpers ---------------------------------------------------------------

lv_obj_t *blankPage(lv_obj_t *parent) {
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, kW, kH);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(kBg), 0);
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
void statusBar(lv_obj_t *page) {
    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kW, kBarH);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_STATUS_BAR_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    // Decorative strip, not a target: it spans the full width and would
    // otherwise sit in the input chain over the top of the page.
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    // Small divider so the bar reads as chrome rather than part of the content.
    lv_obj_t *rule = lv_obj_create(bar);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, kW, 1);
    lv_obj_set_pos(rule, 0, kBarH - 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(0x2A2A3C), 0);
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
    lv_obj_set_style_line_color(slash, lv_color_hex(kBad), 0);
    lv_obj_clear_flag(slash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(slash, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ssid = lv_label_create(bar);
    lv_label_set_text(ssid, "no wifi");
    lv_obj_set_style_text_color(ssid, lv_color_hex(UI_STATUS_BAR_FG), 0);
    lv_obj_set_style_text_font(ssid, &lv_font_montserrat_14, 0);
    lv_obj_align(ssid, LV_ALIGN_LEFT_MID, 46, 0);

    // Register this page's copy so setWifiBar() updates ALL of them, not just
    // the last one built. See the StatusBarRefs comment for why this matters.
    if (s_barCount < kMaxBars) {
        s_bars[s_barCount].icon  = icon;
        s_bars[s_barCount].slash = slash;
        s_bars[s_barCount].ssid  = ssid;
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

    // Only redraw when something the face actually depends on has changed.
    // Without this the clock would invalidate its region on every loop pass,
    // which is exactly the redraw storm that previously starved input.
    static uint8_t lastS = 0xFF, lastM = 0xFF, lastH = 0xFF;
    static int lastReady = -1;
    const int readyNow = ready ? 1 : 0;
    if (readyNow == lastReady && t.second == lastS && t.minute == lastM &&
        t.hour == lastH) {
        return;
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

    s_sumServer = label(s_summary, "-", 0, 0, kMuted, &lv_font_montserrat_14);
    lv_obj_align(s_sumServer, LV_ALIGN_TOP_MID, 0, kBarH + 112);

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
    s_homeClock = label(s_home, "--:--:--", UI_CLOCK_X, UI_CLOCK_Y + UI_CLOCK_DIAMETER + 6,
                        kMuted, &lv_font_montserrat_20);

    s_homeSync = label(s_home, "belum sinkron", UI_CLOCK_X,
                       UI_CLOCK_Y + UI_CLOCK_DIAMETER + 32, kWarn,
                       &lv_font_montserrat_12);
    // Start hidden, shown by updateClock() when there is no time.
    lv_obj_add_flag(s_homeSync, LV_OBJ_FLAG_HIDDEN);

    // Branch name, right-hand side.
    s_homeBranch = label(s_home, "Indorep", 250, kBarH + 90, kText,
                         &lv_font_montserrat_32);

    // ---------------- QRIS ----------------
    s_qris = blankPage(s_scr);
    statusBar(s_qris);
    s_qrTitle = label(s_qris, "QRIS", 24, kBarH + 10, kText,
                      &lv_font_montserrat_16);

    // The CANVAS size is passed, not the on-screen footprint: qr::create() adds
    // the quiet-zone padding itself. Passing kQrSize here would double it.
    s_qrObj = qr::create(s_qris, kQrX + kQrQuiet, kQrY + kQrQuiet, kQrCanvas);

    s_qrItems = lv_label_create(s_qris);
    lv_label_set_text(s_qrItems, "");
    lv_obj_set_style_text_color(s_qrItems, lv_color_hex(kText), 0);
    // 12 px: the cart can be several lines, and the font must be small enough
    // that the longest realistic basket still fits the left column.
    lv_obj_set_style_text_font(s_qrItems, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(s_qrItems, 20, kBarH + 32);
    lv_obj_set_width(s_qrItems, kQrX - 30);
    lv_label_set_long_mode(s_qrItems, LV_LABEL_LONG_WRAP);

    s_qrTotal = lv_label_create(s_qris);
    lv_label_set_text(s_qrTotal, "");
    lv_obj_set_style_text_color(s_qrTotal, lv_color_hex(kGood), 0);
    lv_obj_set_style_text_font(s_qrTotal, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_qrTotal, 20, kQrY + kQrSize - 22);

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

void showSummary(const char *ssid, bool serverOk) {
    if (s_sumSsid) lv_label_set_text(s_sumSsid, ssid ? ssid : "-");

    // Server state is the only technical line left on this screen. It is shown
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

    if (s_qrItems) {
        char buf[512];
        int n = 0;
        if (payload.item_count == 0) {
            n += snprintf(buf + n, sizeof(buf) - n, "(tanpa rincian item)\n");
        }
        for (int i = 0; i < payload.item_count && n < (int)sizeof(buf) - 48; i++) {
            n += snprintf(buf + n, sizeof(buf) - n, "%s  x%d\n",
                          payload.items[i].item, payload.items[i].qty);
        }
        lv_label_set_text(s_qrItems, buf);
    }

    if (s_qrTotal) {
        lv_label_set_text(s_qrTotal, payload.total[0] ? payload.total : "");
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
            if (s_qrTitle) lv_label_set_text(s_qrTitle, "QR GAGAL DIBUAT");
        } else if (qr::update(s_qrObj, payload.qr)) {
            qr::setVisible(s_qrObj, true);
            if (s_qrTitle) lv_label_set_text(s_qrTitle, "SCAN UNTUK BAYAR");
        } else {
            Serial.printf("[ui] QR encode failed (wanted %dpx in a %dpx canvas)\n",
                          wantPx, kQrCanvas);
            qr::setVisible(s_qrObj, false);
            if (s_qrTitle) lv_label_set_text(s_qrTitle, "QR GAGAL DIBUAT");
        }
    } else {
        qr::setVisible(s_qrObj, false);
        if (s_qrTitle) lv_label_set_text(s_qrTitle, "TOTAL BAYAR");
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
            lv_obj_set_style_text_color(b.icon,
                                        lv_color_hex(connected ? kGood : kMuted), 0);
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
