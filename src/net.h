// net.h — Wi-Fi station bring-up for WARNET / CAFE / IDRP(debug).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"

namespace net {

// Human-readable SSID for a menu choice.
const char *ssidFor(NetChoice choice);

// Begin an association attempt. Non-blocking: completion is observed via poll().
void beginConnect(NetChoice choice);

// Drive an in-flight attempt. Call from the main loop. Returns true when the
// station has an IP (whether it just got it or already had it).
bool poll();

// True while an attempt is in flight.
bool connecting();

// True if the most recent attempt failed (timeout).
bool failed();

// Short human-readable reason for the most recent failure, derived from the
// ESP-IDF station status (e.g. "AP tidak ditemukan", "password salah").
const char *failureReason();

// Latest assigned address as a dotted string.
const char *ipString();

// SSID actually associated with (falls back to the requested one).
const char *activeSsid();

// RSSI in dBm, or 0 when not connected.
int rssi();

// True when the station currently holds an IP.
bool connected();

// True when the address this device holds differs from the one the Go backend
// is configured to call (NET_BACKEND_ADDR).
//
// This is a NEUTRAL COMPARISON, not a fault report, and it is named accordingly
// at the call sites. The firmware cannot know:
//   * whether the backend has since been pointed at this device's address, or
//   * whether another host on the backend's address forwards traffic here.
// So callers must present it as information ("the target differs"), never as
// "unreachable" — an earlier version did the latter and it was misleading: a
// perfectly healthy device displayed a red error because the *network* had a
// routing problem the firmware could not see.
bool backendUnreachable();

// Drop the connection and return to idle.
void disconnect();

// One-line status for the log, e.g. "WARNET 192.168.4.254 rssi=-58".
const char *statusLine();

// The station MAC address as "AA:BB:CC:DD:EE:FF".
//
// This is a HARDWARE constant (from the eFuse), not something DHCP assigns, so
// it is valid before any connection and is what you paste into a router's DHCP
// reservation / static-lease table.
const char *macString();

// --- Wall-clock time (NTP) --------------------------------------------------
//
// The standby clock is the only consumer. Time is synced over NTP once the
// station is associated; until then timeReady() stays false and the UI must show
// an explicit "not synced" state rather than a plausible-looking wrong time.
//
// WHY THERE IS A service* FUNCTION AND NOT JUST begin*()
//
// configTime() is not fire-and-forget on this core. It configures the SNTP
// client, but if DNS or the socket is not usable YET when it is called, that
// first attempt fails and nothing re-issues it: the clock then sits at
// "unsynced" for the rest of the boot. That is the observed "sometimes it shows
// the time, sometimes it is stuck on belum sinkron".
//
// So the timing is driven from the main loop: beginTimeSync() arms the intent,
// and serviceTimeSync() (called every pass) waits for DNS to be ready, then
// (re)issues configTime() until a plausible time actually arrives. Nothing here
// blocks.

// Arm the NTP sync. Idempotent. Call once the link is up. Does not itself start
// the sync: serviceTimeSync() does that, so the first attempt can wait for DNS.
void beginTimeSync();

// Drive the sync. Call every pass of the main loop. Non-blocking. Returns
// immediately once the clock is synced.
void serviceTimeSync();

// True once the system clock has been set from NTP and is plausible.
bool timeReady();

// Current local time, honouring NTP_TZ_OFFSET_S. Only meaningful when
// timeReady() is true, but it always fills the struct with whatever the system
// clock believes, so callers can render a face either way.
struct TimeOfDay {
    uint8_t hour;    // 0..23
    uint8_t minute;  // 0..59
    uint8_t second;  // 0..59
};
TimeOfDay localTime();

}  // namespace net
