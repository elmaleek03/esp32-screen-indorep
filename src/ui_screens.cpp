// ui_screens.cpp — the product UI. See ui_screens.h for the threading contract.
//
// Layout is designed for 480x272 exactly, no scrolling: this is a fixed panel,
// not a phone. Every screen is a plain lv_obj child of lv_scr_act() that is
// shown/hidden, rather than a separate screen, so the LVGL tree is built once
// at boot and never allocated/freed during operation (no heap churn, no
// use-after-free on a transition mid-render).
#include "ui_screens.h"

#include <Arduino.h>
#include <lvgl.h>
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

// Palette: dark slate background, cyan accents, matches the bring-up look.
constexpr uint32_t kBg      = 0x101018;
constexpr uint32_t kPanel   = 0x1B1B28;
constexpr uint32_t kAccent  = 0x00C8FF;
constexpr uint32_t kGood    = 0x35D07F;
constexpr uint32_t kWarn    = 0xE0A030;
constexpr uint32_t kBad     = 0xE04A4A;
constexpr uint32_t kText    = 0xE8E8F0;
constexpr uint32_t kMuted   = 0x9090A8;
// Pressed-state background for every button: a clearly distinct lighter tone, so
// a working touchscreen gives obvious feedback on contact.
constexpr uint32_t kBtnPressed = 0xF0F0FF;

// QRIS page geometry. These are namespace-scope because showQris() (below)
// needs the same numbers to report an encode shortfall against the real canvas
// size. The canvas size is FIXED: lv_qrcode allocates its buffer once, in the
// constructor, and lv_obj_set_size() afterwards would not reallocate it.
// 216 px leaves room for the 30 px header, a caption, and the total line on a
// 272 px tall panel, with the summary in the left column.
constexpr int kQrSize = 216;
constexpr int kQrX    = kW - kQrSize - 12;
constexpr int kQrY    = 40;

Screen s_current = Screen::BOOT_MENU;

lv_obj_t *s_scr        = nullptr;

// --- boot menu ---
lv_obj_t *s_menu       = nullptr;
lv_obj_t *s_menuBanner = nullptr;   // red line under the buttons, set on failure
// --- connecting ---
lv_obj_t *s_connect    = nullptr;
lv_obj_t *s_connectDots= nullptr;   // "..." / ".." / "." progress indicator
lv_obj_t *s_connectSsid= nullptr;
lv_obj_t *s_connectStat= nullptr;
// --- confirm ---
lv_obj_t *s_confirm    = nullptr;
lv_obj_t *s_confSsid   = nullptr;
lv_obj_t *s_confIp     = nullptr;
lv_obj_t *s_confMac    = nullptr;
lv_obj_t *s_confSrv    = nullptr;
lv_obj_t *s_confTarget = nullptr;   // where the backend knocks + reachability
// --- home ---
lv_obj_t *s_home       = nullptr;
lv_obj_t *s_homeStat   = nullptr;
lv_obj_t *s_homeClock  = nullptr;
// --- qris ---
lv_obj_t *s_qris       = nullptr;
lv_obj_t *s_qrObj      = nullptr;
lv_obj_t *s_qrItems    = nullptr;
lv_obj_t *s_qrTotal    = nullptr;
lv_obj_t *s_qrTitle    = nullptr;

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

struct BtnCtx {
    int action;   // NetChoice value, or a UI sentinel
};

// Sentinels that cannot collide with NetChoice (which is 0..3).
constexpr int kActFinish = 100;
constexpr int kActBack   = 101;
constexpr int kActCancel = 102;

void onMenuButton(lv_event_t *e) {
    const int action = (int)(intptr_t)lv_event_get_user_data(e);
    if (action == kActBack) {
        showBootMenu();
        return;
    }
    if (action == kActCancel) {
        Serial.println("[ui] connecting -> Cancel (aborting association)");
        uiRequestBack();   // main.cpp stops the attempt and returns to the menu
        return;
    }
    // NetChoice values are 1..3.
    NetChoice choice = (NetChoice)action;
    Serial.printf("[ui] menu tap -> \"%s\"\n", net::ssidFor(choice));
    uiRequestConnect(choice);
}

void onConfirmButton(lv_event_t *e) {
    const int action = (int)(intptr_t)lv_event_get_user_data(e);
    if (action == kActBack) {
        Serial.println("[ui] confirm -> Back (returning to menu)");
        uiRequestBack();
    } else if (action == kActFinish) {
        Serial.println("[ui] confirm -> Finish (entering standby)");
        showHome();
    }
}

void onHomeTap(lv_event_t *e) {
    (void)e;
    // Tapping the standby screen returns to the menu so the operator can change
    // APs without power-cycling. Deliberate: the web API is the normal path, but
    // a physical escape hatch matters when the panel is mounted in a shop.
    showBootMenu();
}

lv_obj_t *makeButton(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                     lv_coord_t w, lv_coord_t h, uint32_t bg, int action,
                     lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(parent);

    // Do NOT use lv_obj_remove_style_all() here. It also strips the theme's
    // LV_STATE_PRESSED styling, which means tapping a button produces no visual
    // feedback at all — on a device whose headline symptom is "touch does not
    // work", an unresponsive-looking button is actively misleading. Instead keep
    // the theme's default (which already gives the pressed/inactive states) and
    // only recolour the background.
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(b, lv_color_hex(kBtnPressed), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    // Collapse the theme's press transition so the colour change is immediate.
    lv_obj_set_style_bg_color(b, lv_color_hex(kBtnPressed),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)action);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return b;
}

void hideAll() {
    lv_obj_t *pages[] = {s_menu, s_connect, s_confirm, s_home, s_qris};
    for (lv_obj_t *p : pages) {
        if (p) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    }
}

// Connecting-screen progress dots.
//
// One label, rewritten only when its text actually changes, at 3 Hz. That is the
// cheapest possible "the device is alive" indicator: a single small label
// invalidated ~3 times a second, versus an animated widget redrawing at LVGL's
// full refresh rate forever.
void tickConnectingDots() {
    if (s_connectDots == nullptr) return;

    static uint32_t last = 0;
    if (millis() - last < 330) return;
    last = millis();

    static uint8_t phase = 0;
    phase = (uint8_t)((phase + 1) % 4);
    // "...." -> "..." -> ".." -> "." keeps the label's width stable enough that
    // the redraw region does not jump around.
    static const char *frames[] = {".", "..", "...", "...."};
    const char *want = frames[phase];

    const char *cur = lv_label_get_text(s_connectDots);
    if (cur != nullptr && strcmp(cur, want) == 0) return;
    lv_label_set_text(s_connectDots, want);
}

void show(lv_obj_t *page, Screen which) {
    // Order matters, and this ordering is the fix for "after navigating, touch
    // stops working":
    //
    //  1. Release the touch press FIRST, before any widget changes. LVGL's indev
    //     tracks the object it is currently pressing. If the widget tree changes
    //     while that pointer is set, the RELEASED event goes to a widget on the
    //     screen we just hid, the indev's press never completes, and the new
    //     screen receives no input whatsoever. Dropping the press before the
    //     swap means the next input lands cleanly on the new screen.
    touch::forceRelease();

    hideAll();
    if (page) {
        lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(page);
    }
    s_current = which;
    Serial.printf("[ui] screen -> %d\n", (int)which);
}

// Draw a small header strip used by menu/connecting/confirm.
void header(lv_obj_t *page, const char *title, uint32_t accent) {
    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kW, 30);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    // Decorative strip, not a target: it spans the full width and would
    // otherwise sit in the input chain over the top 30 px of the page.
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(bar);
    lv_label_set_text(l, title);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
}

}  // namespace

void build() {
    s_scr = lv_scr_act();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(kBg), 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---------------- BOOT MENU ----------------
    // Layout budget: 480x272, no scrolling. Header occupies 0..30.
    //   y=38..56   prompt
    //   y=62..122  three AP buttons (60 tall)
    //   y=132..152 two explanatory lines (14px)
    //   y=182+     failure banner (empty unless a connect attempt failed)
    s_menu = blankPage(s_scr);
    header(s_menu, "INDOREP QRIS DISPLAY", kAccent);
    label(s_menu, "Pilih jaringan:", 16, 38, kMuted);

    makeButton(s_menu, "WARNET", 16, 62, 140, 60, 0x2E7D32,
               (int)NetChoice::WARNET, onMenuButton);
    makeButton(s_menu, "CAFE", 170, 62, 140, 60, 0x1565C0,
               (int)NetChoice::CAFE, onMenuButton);
    makeButton(s_menu, "debug", 324, 62, 140, 60, 0x5A5A78,
               (int)NetChoice::DEBUG, onMenuButton);

    label(s_menu, "WARNET / CAFE: open networks", 16, 132, kMuted);
    label(s_menu, "debug: \"IDRP\" WPA2. Device then serves HTTP on :80.",
          16, 152, kMuted);

    // Failure banner. Empty (and visually absent) until a connect attempt fails.
    s_menuBanner = label(s_menu, "", 16, 182, kBad);
    lv_obj_set_width(s_menuBanner, kW - 32);
    lv_label_set_long_mode(s_menuBanner, LV_LABEL_LONG_WRAP);

    // ---------------- CONNECTING ----------------
    s_connect = blankPage(s_scr);
    header(s_connect, "MENGHUBUNGKAN", kWarn);
    s_connectSsid = label(s_connect, "-", 16, 52, kText, &lv_font_montserrat_20);

    // Progress indication is TEXT, not an animated widget.
    //
    // lv_spinner animates continuously for as long as the screen is visible, and
    // every animation step is a redraw of that region — forever, at LVGL's
    // refresh rate. It is the single busiest thing this firmware could do, for
    // information ("something is happening") that three dots convey just as
    // well. See updateConnectingDots(): it rewrites ONE label, only when the
    // text actually changes, three times a second.
    s_connectDots = label(s_connect, "...", 16, 92, kText, &lv_font_montserrat_20);
    lv_obj_set_width(s_connectDots, 200);

    s_connectStat = label(s_connect, "memulai...", 16, 124, kMuted);
    lv_obj_set_width(s_connectStat, 380);
    lv_label_set_long_mode(s_connectStat, LV_LABEL_LONG_WRAP);

    // Cancel design notes:
    //
    //  * Created AFTER everything else on this page, so nothing can sit on top
    //    of it in the input chain.
    //  * Large (200x64) and placed low and left, away from any decoration, so it
    //    is an easy target when the operator is in a hurry.
    makeButton(s_connect, "Batal", 16, 180, 200, 64, 0x9A4A4A, kActCancel,
               onMenuButton);

    // ---------------- CONFIRM ----------------
    s_confirm = blankPage(s_scr);
    header(s_confirm, "TERHUBUNG", kGood);
    label(s_confirm, "SSID", 16, 38, kMuted);
    s_confSsid = label(s_confirm, "-", 100, 38, kText, &lv_font_montserrat_20);
    label(s_confirm, "IP", 16, 68, kMuted);
    s_confIp = label(s_confirm, "-", 100, 68, kText, &lv_font_montserrat_20);

    // The MAC is shown here deliberately: it is the value to paste into a
    // router's DHCP reservation, and reading it off the panel means the device
    // does not have to be unplugged and put on a serial console to identify it.
    label(s_confirm, "MAC", 16, 98, kMuted);
    s_confMac = label(s_confirm, "-", 100, 98, kText, &lv_font_montserrat_14);

    label(s_confirm, "Server", 16, 122, kMuted);
    s_confSrv = label(s_confirm, "-", 100, 122, kText);

    // Where the backend will actually knock. If the device is NOT on this
    // address the backend cannot reach it, and that is a SILENT failure: the
    // device looks healthy while every /update times out after 5 s.
    label(s_confirm, "Target", 16, 148, kMuted);
    s_confTarget = label(s_confirm, "-", 100, 148, kMuted);

    makeButton(s_confirm, "Back", 16, 196, 140, 52, 0x5A5A78, kActBack,
               onConfirmButton);
    makeButton(s_confirm, "Finish", 324, 196, 140, 52, 0x2E7D32, kActFinish,
               onConfirmButton);

    // ---------------- HOME (standby) ----------------
    s_home = blankPage(s_scr);
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_home, onHomeTap, LV_EVENT_CLICKED, nullptr);
    header(s_home, "INDOREP", kAccent);

    lv_obj_t *ready = lv_label_create(s_home);
    lv_label_set_text(ready, LV_SYMBOL_OK "  SIAP");
    lv_obj_set_style_text_color(ready, lv_color_hex(kGood), 0);
    lv_obj_set_style_text_font(ready, &lv_font_montserrat_32, 0);
    lv_obj_align(ready, LV_ALIGN_CENTER, 0, -10);

    s_homeStat = label(s_home, "menunggu transaksi...", 16, 200, kMuted);
    s_homeClock = label(s_home, "uptime 0s", 16, 226, kMuted);

    // ---------------- QRIS ----------------
    s_qris = blankPage(s_scr);
    header(s_qris, "SCAN UNTUK BAYAR", kAccent);
    s_qrTitle = label(s_qris, "QRIS", 16, 40, kText, &lv_font_montserrat_20);

    // QR on the right, summary on the left (geometry at namespace scope above).
    s_qrObj = qr::create(s_qris, kQrX, kQrY, kQrSize);

    s_qrItems = lv_label_create(s_qris);
    lv_label_set_text(s_qrItems, "");
    lv_obj_set_style_text_color(s_qrItems, lv_color_hex(kText), 0);
    lv_obj_set_style_text_font(s_qrItems, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_qrItems, 16, 70);
    lv_obj_set_width(s_qrItems, kQrX - 28);
    lv_label_set_long_mode(s_qrItems, LV_LABEL_LONG_WRAP);

    s_qrTotal = lv_label_create(s_qris);
    lv_label_set_text(s_qrTotal, "");
    lv_obj_set_style_text_color(s_qrTotal, lv_color_hex(kGood), 0);
    lv_obj_set_style_text_font(s_qrTotal, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_qrTotal, 16, kQrY + kQrSize - 30);

    showBootMenu();
}

void showBootMenu() { show(s_menu, Screen::BOOT_MENU); }

void showConnecting(const char *ssid) {
    if (s_connectSsid) lv_label_set_text(s_connectSsid, ssid ? ssid : "-");
    if (s_connectStat) lv_label_set_text(s_connectStat, "memulai...");
    // A new attempt clears the previous failure banner.
    if (s_menuBanner) lv_label_set_text(s_menuBanner, "");
    show(s_connect, Screen::CONNECTING);
}

void showConnectFailed(const char *ssid, const char *reason) {
    char buf[192];
    snprintf(buf, sizeof(buf), "GAGAL: tidak bisa terhubung ke \"%s\" (%s)",
             ssid ? ssid : "-", reason ? reason : "-");
    if (s_menuBanner) lv_label_set_text(s_menuBanner, buf);
    Serial.printf("[ui] connection failure banner: %s\n", buf);
    showBootMenu();
}

void setConnectingStatus(const char *text) {
    if (s_connectStat == nullptr || text == nullptr) return;

    // NEVER write an unchanged string. lv_label_set_text() marks the label dirty
    // unconditionally, and a dirty label triggers a redraw on the next LVGL
    // timer run — so writing the same text in a loop is a permanent redraw
    // storm that starves touch input. Comparing first makes this call free when
    // nothing changed, whatever the caller's rate.
    const char *cur = lv_label_get_text(s_connectStat);
    if (cur != nullptr && strcmp(cur, text) == 0) return;

    lv_label_set_text(s_connectStat, text);
}

void showConfirm(const char *ssid, const char *ip, bool serverOk) {
    if (s_confSsid) lv_label_set_text(s_confSsid, ssid ? ssid : "-");
    if (s_confIp)   lv_label_set_text(s_confIp, ip ? ip : "-");
    if (s_confMac)  lv_label_set_text(s_confMac, net::macString());

    if (s_confSrv) {
        lv_label_set_text(s_confSrv, serverOk ? "RUNNING :80" : "FAILED");
        lv_obj_set_style_text_color(s_confSrv,
                                    lv_color_hex(serverOk ? kGood : kBad), 0);
    }

    // Show where the backend is configured to look. This is INFORMATION, not a
    // fault report.
    //
    // An earlier version printed "UNREACHABLE from backend!" in red whenever this
    // device's address differed from NET_BACKEND_ADDR. That was wrong: the
    // firmware cannot know where the backend actually points (it may have been
    // updated), and it cannot know whether another host on .254 forwards traffic
    // here. All it can honestly state is the configured target, so that is what
    // it shows. Whether traffic arrives is a network question, answered by
    // /health on the backend side, not by guessing here.
    if (s_confTarget) {
        const bool sameAddr = !net::backendUnreachable();
        lv_label_set_text_fmt(s_confTarget, "%s%s", NET_BACKEND_ADDR,
                              sameAddr ? "  (= this device)" : "  (different)");
        lv_obj_set_style_text_color(s_confTarget,
                                    lv_color_hex(sameAddr ? kGood : kMuted), 0);
    }

    show(s_confirm, Screen::CONFIRM);
}

void showHome() {
    show(s_home, Screen::HOME);
    tick();
}

void showQris(const PaymentPayload &payload) {
    if (s_qrItems) {
        char buf[512];
        int n = 0;
        if (payload.item_count == 0) {
            n += snprintf(buf + n, sizeof(buf) - n, "(tanpa rincian item)\n");
        }
        for (int i = 0; i < payload.item_count && n < (int)sizeof(buf) - 48; i++) {
            n += snprintf(buf + n, sizeof(buf) - n, "%s\n   x%d\n",
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
        // payload can be encoded at all and how many pixels it would want, which
        // makes a "QR failed" report immediately actionable in the log.
        const int wantPx = qr::requiredSize(payload.qr);
        if (wantPx <= 0) {
            Serial.println("[ui] payload exceeds QR capacity");
            qr::setVisible(s_qrObj, false);
            if (s_qrTitle) lv_label_set_text(s_qrTitle, "QR GAGAL DIBUAT");
        } else if (qr::update(s_qrObj, payload.qr)) {
            qr::setVisible(s_qrObj, true);
            if (s_qrTitle) lv_label_set_text(s_qrTitle, "QRIS - SCAN");
        } else {
            // Encodable in principle but the canvas rejected it: report the
            // shortfall rather than showing a stale code that would mislead.
            Serial.printf("[ui] QR encode failed (wanted %dpx in a %dpx canvas)\n",
                          wantPx, kQrSize);
            qr::setVisible(s_qrObj, false);
            if (s_qrTitle) lv_label_set_text(s_qrTitle, "QR GAGAL DIBUAT");
        }
    } else {
        qr::setVisible(s_qrObj, false);
        if (s_qrTitle) lv_label_set_text(s_qrTitle, "TOTAL BAYAR");
    }

    show(s_qris, Screen::QRIS);
}

void setHomeStatus(const char *text) {
    if (s_homeStat == nullptr || text == nullptr) return;

    // Same reasoning as setConnectingStatus(): a redundant lv_label_set_text()
    // is a redraw, and at loop rate that is a redraw storm. Skip when equal.
    const char *cur = lv_label_get_text(s_homeStat);
    if (cur != nullptr && strcmp(cur, text) == 0) return;

    lv_label_set_text(s_homeStat, text);
}

Screen current() { return s_current; }

void tick() {
    if (s_current == Screen::CONNECTING) {
        tickConnectingDots();
        return;
    }
    if (s_current != Screen::HOME || s_homeClock == nullptr) return;

    static uint32_t last_s = 0;
    const uint32_t now_s = millis() / 1000;
    if (now_s == last_s) return;   // only touch the label when it changes
    last_s = now_s;

    char buf[64];
    snprintf(buf, sizeof(buf), "uptime %luh %lum %lus  heap %uK",
             (unsigned long)(now_s / 3600), (unsigned long)((now_s / 60) % 60),
             (unsigned long)(now_s % 60), (unsigned)(ESP.getFreeHeap() / 1024));
    lv_label_set_text(s_homeClock, buf);
}

}  // namespace ui
