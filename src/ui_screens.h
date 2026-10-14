// ui_screens.h — the production UI.
//
// Flow:
//   BOOT_SCREEN -> WIFI_SELECT -> CONNECTING -> SUMMARY -> HOME <-> QRIS
//
// THREADING CONTRACT
//
// LVGL v8 is not thread-safe, and no amount of locking makes it so. Rather than
// pretend otherwise, this firmware serialises ALL widget access onto the main
// loop task: every function below is called from main.cpp's loop()/setup() only.
//
// The web server never calls into this module. It publishes a payload into its
// own buffer and raises a flag; main.cpp notices during its pass and calls
// ui::showQris() itself. That is why every entry point here is documented as
// "main-loop task only".
#pragma once

#include <stdbool.h>

#include "app_state.h"

namespace ui {

// Create every screen once, at boot. Must run after display::begin().
void build();

// --- Main-loop-task-only screen transitions --------------------------------

// The cosmetic boot log. Reveals one line at a time from tick() and advances to
// WIFI_SELECT on its own. All real initialisation has ALREADY happened by the
// time this is shown, so it can never mask an init failure.
void showBootScreen();

// True while the boot log is still revealing lines. main.cpp uses this to hold
// off anything that would steal the screen early.
bool bootScreenActive();

// Choose WARNET / CAFE, with a small Debug button bottom-right.
void showWifiSelect();

// Associating. Shows the SSID and a dots indicator.
void showConnecting(const char *ssid);

// Association failed. Returns to WIFI_SELECT with a red banner naming the cause.
void showConnectFailed(const char *ssid, const char *reason);

// Big green check, wifi info (SSID + DHCP-assigned IP), server state, and a
// Continue button. Pressing Continue LATCHES the route: WIFI_SELECT becomes
// unreachable for the life of this boot.
void showSummary(const char *ssid, const char *ip, bool serverOk);

// The standby homescreen: analog clock + branch name. Only reachable after the
// route is latched.
void showHome();

// Render the QRIS page from `payload`. When payload.has_qr is false the QR is
// hidden and only the summary is shown (cash flow).
void showQris(const PaymentPayload &payload);

// Update the connecting screen's status line (e.g. "associating...").
void setConnectingStatus(const char *text);

// Update the shared top status bar on every screen except the boot log.
// Called from the main loop; internally change-detected so a steady state costs
// nothing (a redundant lv_label_set_text() is a redraw, and at loop rate that is
// what previously starved touch input).
void setWifiBar(bool connected, const char *ssid);

// Which screen is currently visible.
Screen current();

// True once the operator has pressed Continue and the route is committed.
bool routeLatched();

// Called from the main loop to refresh anything time-based (boot log reveal,
// connecting dots, the clock). Cheap when nothing changed.
void tick();

}  // namespace ui
