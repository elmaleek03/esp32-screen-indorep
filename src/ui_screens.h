// ui_screens.h — the product UI: boot menu, connecting, confirm, home, QRIS.
//
// THREADING CONTRACT
//
// LVGL v8 is not thread-safe, and no amount of locking makes it so. Rather than
// pretend otherwise, this firmware serialises ALL widget access onto the main
// loop task: every function below is called from main.cpp's loop()/setup() only.
//
// The web server (which runs in its own task) therefore never calls into this
// module directly. It publishes a payload into web_server's own buffer and
// raises a flag; main.cpp notices the flag during its pass and calls
// ui::showQris()/ui::showHome() itself. That is why every entry point here is
// documented as "main-loop task only".
#pragma once

#include <stdbool.h>

#include "app_state.h"

namespace ui {

// Create every screen once, at boot. Must run after display::begin().
void build();

// --- Main-loop-task-only screen transitions --------------------------------

// Switch to the boot menu.
void showBootMenu();

// Switch to the connecting screen for `ssid`.
void showConnecting(const char *ssid);

// Record a failed association and show the boot menu. The reason is then drawn
// as a persistent red banner on the menu, so the operator can see WHY it failed
// rather than watching the menu reappear (which looks like the tap was never
// registered). Cleared automatically by the next showConnecting().
void showConnectFailed(const char *ssid, const char *reason);

// Switch to the confirm screen. `serverOk` reflects whether the HTTP server
// bound successfully.
void showConfirm(const char *ssid, const char *ip, bool serverOk);

// Switch to the standby homescreen.
void showHome();

// Render the QRIS page from `payload`. When payload.has_qr is false the QR is
// hidden and only the summary is shown (cash flow).
void showQris(const PaymentPayload &payload);

// Update the small live status line on the homescreen (wifi/ip/uptime).
void setHomeStatus(const char *text);

// Update the status line on the connecting screen (e.g. "associating...").
void setConnectingStatus(const char *text);

// Which screen is currently visible.
Screen current();

// Called from the main loop to refresh anything time-based (e.g. uptime on the
// homescreen). Cheap when nothing changed.
void tick();

}  // namespace ui
