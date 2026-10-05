// web_server.h — HTTP API the Go backend talks to.
//
// Contract (from indorep_backend_v2/display/display.go):
//   POST /update  {"qr":"...","cart":[{"qty":1,"item":"..."}],"total_price":"Rp"}
//                 -> 200, must answer within 5 s
//   GET  /reset   -> 200, return to standby
//
// THREADING MODEL — read this before changing anything here.
//
// WebServer callbacks run in the main loop task only if the server is pumped
// from loop(). They are pumped from loop() here (see pump()), so handlers
// actually execute in the LVGL task. Even so, the handlers DO NOT touch LVGL:
// they validate the payload, copy it into a static mailbox, and return. The
// main loop later notices the mailbox and performs the UI transition itself.
//
// This indirection is what makes the firmware safe:
//   * a slow client cannot hold LVGL hostage,
//   * the UI transition always happens between two lv_timer_handler calls, so
//     there is never a widget mutation mid-render,
//   * a second /update arriving before the first is drawn simply replaces the
//     pending payload (last-write-wins), matching what a POS expects.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"

namespace web {

// Start the HTTP server on WEB_SERVER_PORT. Returns false if it failed to bind.
bool begin();

// Pump the server. Call from the main loop, frequently.
void pump();

// True if the server is listening.
bool running();

// --- Mailbox consumed by main.cpp ------------------------------------------

// Non-blocking claim of a pending payload. Returns true (and fills `out`) at
// most once per posted payload.
bool takePending(PaymentPayload *out);

// Non-blocking claim of a pending /reset request.
bool takeResetRequest();

// True if a payload has been posted and not yet claimed.
bool hasPending();

// Number of requests served, for the health log.
uint32_t requestCount();

}  // namespace web
