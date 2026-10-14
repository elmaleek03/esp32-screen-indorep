// net.cpp — Wi-Fi station bring-up for WARNET / CAFE / IDRP(debug).
//
// Behaviour notes:
//  * The device takes its address from DHCP and uses it as-is. There is no
//    static configuration: the earlier DHCP-then-static-fallback scheme to force
//    192.168.4.252 conflicted with the router and left the device unreachable,
//    so it was removed. The assigned address is reported on the connected screen
//    and on /health.
//  * An attempt has a hard deadline (WIFI_CONNECT_TIMEOUT_MS). Without it a
//    wrong/absent AP leaves the UI spinning forever.
//  * Retries are deliberately NOT automatic. A silent reconnect loop hides the
//    failure mode; the operator sees the failure on the confirm screen and can
//    pick a different AP.
#include "net.h"

#include <Arduino.h>
#include <WiFi.h>

#include "board_config.h"
#include "wifi_profiles.h"

namespace net {

// Declared here because beginConnect() log messages reference it; defined at the
// bottom of the file with the other accessors.
const char *macString();

namespace {

NetChoice s_choice  = NetChoice::NONE;
bool      s_active  = false;   // attempt in flight
bool      s_failed  = false;
bool      s_got_ip  = false;
uint32_t  s_started = 0;
char      s_ip[16]  = "0.0.0.0";
char      s_line[96] = "idle";

// Station status captured at the moment of failure, used by failureReason().
wl_status_t s_lastStatus = WL_IDLE_STATUS;

// DHCP ONLY. The address is whatever the router leases; nothing here overrides
// it. See board_config.h for why the earlier static-fallback scheme was retired.
// (NET_FALLBACK_* symbols remain defined for the health endpoint, which reports
// what the backend is configured to call, but they are never applied.)

const char *passFor(NetChoice choice) {
    switch (choice) {
        case NetChoice::WARNET: return WIFI_PASS_WARNET;
        case NetChoice::CAFE:   return WIFI_PASS_CAFE;
        case NetChoice::DEBUG:  return WIFI_PASS_DEBUG;
        default:                return "";
    }
}

void refreshIp() {
    const IPAddress ip = WiFi.localIP();
    snprintf(s_ip, sizeof(s_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

void rebuildLine() {
    if (s_failed) {
        snprintf(s_line, sizeof(s_line), "FAILED (%s)", ssidFor(s_choice));
    } else if (s_got_ip) {
        snprintf(s_line, sizeof(s_line), "%s %s rssi=%d", activeSsid(), s_ip, rssi());
    } else if (s_active) {
        snprintf(s_line, sizeof(s_line), "connecting to %s...", ssidFor(s_choice));
    } else {
        snprintf(s_line, sizeof(s_line), "idle");
    }
}

}  // namespace

// Maps the captured IDF station status to something an operator can act on.
// Declared before poll() because poll() logs it. Defined here (after the
// anonymous-namespace helpers it needs) rather than at the bottom of the file.
//
// NOTE: this core's wl_status_t (WiFiType.h) has no WL_WRONG_PASSWORD — that
// enum member is an ESP8266-ism and does not exist on Arduino core 3.x. A bad
// password therefore surfaces as WL_CONNECT_FAILED / WL_DISCONNECTED.
const char *failureReason() {
    switch (s_lastStatus) {
        case WL_NO_SSID_AVAIL:   return "AP tidak ditemukan";
        case WL_CONNECT_FAILED:  return "koneksi ditolak / password salah";
        case WL_CONNECTION_LOST: return "koneksi terputus";
        case WL_DISCONNECTED:
            // The overwhelmingly common cause here: the AP is in range but
            // association was refused (seen in the field as repeated
            // "Reason: 201 - NO_AP_FOUND" followed by "STA_LEAVING").
            return "AP tidak merespon / di luar jangkauan";
        case WL_STOPPED:         return "Wi-Fi dimatikan";
        default:                 return "timeout";
    }
}

const char *ssidFor(NetChoice choice) {
    switch (choice) {
        case NetChoice::WARNET: return WIFI_SSID_WARNET;
        case NetChoice::CAFE:   return WIFI_SSID_CAFE;
        case NetChoice::DEBUG:  return WIFI_SSID_DEBUG;
        default:                return "-";
    }
}

void beginConnect(NetChoice choice) {
    s_choice  = choice;
    s_active  = true;
    s_failed  = false;
    s_got_ip  = false;
    s_started = millis();
    strncpy(s_ip, "0.0.0.0", sizeof(s_ip));

    // Ensure the STA interface exists before disconnecting, otherwise IDF logs
    // "disconnect(): STA not started! You must call begin first." and the call
    // is a silent no-op. WiFi.begin() below is what actually starts the driver,
    // so on the very first connect there is nothing to tear down.
    if (WiFi.getMode() == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_STA);
    }
    if (WiFi.getMode() != WIFI_MODE_NULL) {
        const wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED || st == WL_IDLE_STATUS) {
            Serial.printf("[net] tearing down previous association (status=%d)\n", (int)st);
            WiFi.disconnect(true, true);
            delay(50);
        }
    }

    // DHCP only: the router assigns the address and we use it as-is. Nothing is
    // configured before WiFi.begin(), so the lease is the single source of truth
    // and the connected screen reports exactly what the router handed out.
    Serial.printf("[net] DHCP (address from the router)\n");
    Serial.printf("[net]   STA MAC %s <- use this for a DHCP reservation if the "
                  "backend needs a fixed address\n", macString());

    const char *ssid = ssidFor(choice);
    const char *pass = passFor(choice);
    Serial.printf("[net] connecting to \"%s\" (%s), timeout %d ms...\n",
                  ssid, pass[0] ? "WPA2" : "open", WIFI_CONNECT_TIMEOUT_MS);

    // Open networks: pass an empty password explicitly. Passing nullptr for an
    // open AP is accepted, but being explicit keeps the intent readable.
    WiFi.begin(ssid, pass[0] ? pass : "");

    rebuildLine();
}

bool connecting() { return s_active; }
bool failed()     { return s_failed; }
bool connected()  { return WiFi.status() == WL_CONNECTED && s_got_ip; }

bool poll() {
    if (!s_active) return s_got_ip;

    // EVERY WiFi.* accessor below takes the ESP-IDF Wi-Fi driver mutex. While
    // the station is associating, the Wi-Fi task holds that mutex for long
    // stretches, so calling these on every loop pass BLOCKS loop() — and a
    // blocked loop() means display::tick() never runs and the UI stops accepting
    // touch. That is why the connecting screen felt dead.
    //
    // The fix is to look at the Wi-Fi state at a human rate (10 Hz) rather than
    // a loop rate. Nothing about association changes faster than that, and the
    // loop stays free to pump LVGL between these reads.
    static uint32_t s_lastCheck = 0;
    if (s_lastCheck != 0 && (millis() - s_lastCheck) < 100) {
        return s_got_ip;
    }
    s_lastCheck = millis();

    // Cache the status ONCE per check: each accessor is a separate locked call.
    const wl_status_t st = WiFi.status();

    if (st == WL_CONNECTED) {
        refreshIp();

        // DHCP assigned us an address. Use it as-is: there is no static override
        // any more (see board_config.h). The address is what the router handed
        // out, and it is reported verbatim on the connected screen and /health.
        //
        // If the Go backend cannot reach this device, that is a ROUTER or BACKEND
        // configuration problem, not something the device can fix by claiming an
        // address it does not own: pin the reservation by MAC, or repoint the
        // backend. The log states the two addresses plainly so the comparison is
        // immediate.
        if (strcmp(s_ip, NET_BACKEND_ADDR) != 0) {
            Serial.printf("[net] note: this device is %s but the backend calls %s; "
                          "the router or the backend must bridge that\n",
                          s_ip, NET_BACKEND_ADDR);
        }

        s_got_ip = true;
        s_active = false;
        s_failed = false;
        // Read the descriptive fields only on the transition to connected, not
        // on every poll.
        Serial.printf("[net] connected: ssid=\"%s\" ip=%s rssi=%d dBm ch=%d gw=%s\n",
                      WiFi.SSID().c_str(), s_ip, (int)WiFi.RSSI(),
                      (int)WiFi.channel(), WiFi.gatewayIP().toString().c_str());
        rebuildLine();
        return true;
    }

    if (millis() - s_started > WIFI_CONNECT_TIMEOUT_MS) {
        s_active = false;
        s_failed = true;
        s_lastStatus = st;
        // WL_NO_SSID_AVAIL / WL_CONNECT_FAILED are the actionable ones;
        // reporting the raw status is the fastest diagnosis.
        Serial.printf("[net] FAILED \"%s\" after %d ms (status=%d = %s)\n",
                      ssidFor(s_choice), WIFI_CONNECT_TIMEOUT_MS,
                      (int)s_lastStatus, failureReason());
        rebuildLine();
        return false;
    }

    // Log progress once per second so a slow association is visible.
    static uint32_t last_log = 0;
    if (millis() - last_log >= 1000) {
        last_log = millis();
        Serial.printf("[net] ...status=%d elapsed=%lums\n", (int)st,
                      (unsigned long)(millis() - s_started));
    }
    return false;
}

const char *ipString() { return s_ip; }

bool backendUnreachable() {
    if (!s_got_ip) return false;
    return strcmp(s_ip, NET_BACKEND_ADDR) != 0;
}

const char *activeSsid() {
    static char buf[40];
    const String s = WiFi.SSID();
    if (s.length() > 0) {
        strncpy(buf, s.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        return buf;
    }
    return ssidFor(s_choice);
}

int rssi() {
    if (WiFi.status() != WL_CONNECTED) return 0;
    return (int)WiFi.RSSI();
}

const char *statusLine() {
    rebuildLine();
    return s_line;
}

// --- Time (NTP) -------------------------------------------------------------
//
// The standby clock is the only consumer. None of this runs on a hot path:
// serviceTimeSync() just checks a few cached values and, at most, re-issues
// configTime(); localTime() only reads the system clock.
//
// The device has NO RTC and no battery, so time is lost on every power cycle.
// That is why the UI needs an explicit "unsynced" state rather than assuming a
// clock is available.
//
// A small state, not one flag. An earlier revision used a single
// s_timeSyncStarted latch and called configTime() exactly once, immediately on
// connect — before DNS/sockets were ready. That is why the clock worked on some
// boots and not others: if the one attempt failed, nothing retried it.
//
//   armed    -> beginTimeSync() was called (the link is up); NTP should sync
//   attempts -> how many configTime() calls have been made (0 = not tried yet)
//   ready    -> a plausible time has arrived; stop doing anything
static bool     s_timeArmed       = false;
static bool     s_timeReady       = false;
static uint32_t s_timeAttemptMs   = 0;   // millis of the last configTime()
static uint32_t s_timeArmedMs     = 0;   // millis when beginTimeSync() was called
static uint32_t s_timeAttempts    = 0;   // number of configTime() calls made

// Debounce for "the link is down". 0 = currently up.
//
// The Wi-Fi driver reports a non-connected status transiently (DHCP renew,
// power-save wake), so a single bad sample must not be treated as a dropout —
// see the comment in serviceTimeSync() for why that mattered.
static uint32_t s_linkDownSince   = 0;
static constexpr uint32_t kLinkDownGraceMs = 3000;

// Last-resort re-arm window.
//
// IMPORTANT: we must NOT re-issue configTime() on a short cycle. configTime()
// calls sntp_stop() then sntp_init(), i.e. it TEARS DOWN and restarts the SNTP
// client. Called every few seconds while a sync is in flight it interrupts the
// exchange before the reply arrives, and the clock then never syncs at all.
// lwIP's SNTP client already polls continuously with backoff once started, so a
// single call is normally enough and self-heals.
//
// So configTime() is called ONCE, after DNS is ready. Only if nothing has arrived
// after this much longer window do we treat it as a lost socket and restart —
// rarely, and late enough not to interrupt a slow but working sync.
static constexpr uint32_t kTimeReArmMs = 60000;

// Give DNS this long to become usable after the link is up before the first
// attempt. A hostname NTP server cannot be resolved without it. If the platform
// never reports a DNS server, we still attempt after this grace period (the
// third server is a raw IP and needs no DNS).
static constexpr uint32_t kTimeDnsGraceMs = 2000;

void beginTimeSync() {
    if (s_timeArmed) return;
    s_timeArmed = true;
    s_timeArmedMs = millis();

    // Log the intent, not the fact: the actual configTime() happens in
    // serviceTimeSync() once DNS is ready, so a failure there is visible rather
    // than silent.
    Serial.printf("[net] NTP armed (%s, %s, %s), TZ offset %+ld s\n",
                  NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3,
                  (long)NTP_TZ_OFFSET_S);
}

void serviceTimeSync() {
    if (!s_timeArmed || s_timeReady) return;

    // CHEAP GATE FIRST, then touch the Wi-Fi driver. These accessors take the
    // ESP-IDF driver mutex and this runs on every loop pass, so the millis()
    // check comes first and the driver is only read when an attempt is due.
    //
    // The FIRST attempt is not delayed at all: as soon as the caller has armed
    // us the link is up, so we try immediately. Waiting a second here (as an
    // earlier revision did) only created a window in which a transient Wi-Fi
    // status blip could restart the countdown and keep configTime() from ever
    // being reached. The DNS-readiness concern is handled separately below.
    const bool firstAttempt = (s_timeAttempts == 0);
    if (!firstAttempt && (millis() - s_timeAttemptMs) < kTimeReArmMs) return;

    // While still on the first attempt, don't hammer the driver: probe at most
    // every 250 ms until DNS is ready or the grace period expires. After that
    // (or on a retry) the kTimeReArmMs gate above already spaces the reads out.
    static uint32_t s_firstProbeMs = 0;
    if (firstAttempt) {
        if (s_firstProbeMs != 0 && (millis() - s_firstProbeMs) < 250) return;
        s_firstProbeMs = millis();
    }

    // Read the driver ONCE and cache it. Used for both the down-check below and
    // the DNS read further down.
    const wl_status_t st = WiFi.status();

    // Link health, using the module's own sticky notion of "connected" first.
    //
    // DISARMING IS DEBOUNCED, and this matters: the driver reports
    // WL_DISCONNECTED / WL_IDLE_STATUS for brief moments during normal operation
    // (DHCP renew, power-save wake). An earlier revision disarmed on ANY single
    // non-connected pass and reset s_timeAttempts, which meant configTime() could
    // be starved by blips and never called at all. So a blip must NOT drop the
    // arm; only a sustained loss should.
    if (st != WL_CONNECTED) {
        if (s_linkDownSince == 0) {
            s_linkDownSince = millis();
            return;   // first blip: ignore, keep the arm
        }
        if (millis() - s_linkDownSince < kLinkDownGraceMs) {
            return;   // still within the grace window: keep waiting
        }
        // Sustained: the link really is down. Drop the arm so a later connect can
        // re-arm us; nothing to attempt until then.
        s_timeArmed = false;
        s_timeAttempts = 0;
        s_timeAttemptMs = millis();
        return;
    }
    // Link is up: reset the debounce.
    s_linkDownSince = 0;

    // On the FIRST attempt, prefer to wait for DNS so the hostname servers
    // resolve. Bounded by the grace period, after which we attempt anyway.
    const IPAddress dns = WiFi.dnsIP();
    const bool haveDns = (dns[0] != 0 || dns[1] != 0);
    if (firstAttempt && !haveDns && (millis() - s_timeArmedMs) < kTimeDnsGraceMs) {
        return;
    }

    s_timeAttemptMs = millis();
    s_timeAttempts++;

    Serial.printf("[net] NTP %s #%lu (dns=%s%s)\n",
                  firstAttempt ? "start" : "re-arm",
                  (unsigned long)s_timeAttempts, dns.toString().c_str(),
                  haveDns ? "" : " [NO DNS YET]");

    // TZ offset is passed in seconds, matching NTP_TZ_OFFSET_S (WIB, UTC+7).
    // Passing it here means getLocalTime() reports local time, so there is
    // exactly one definition of "local" in the firmware.
    //
    // Three servers is the maximum configTime() accepts. The first two are
    // hostnames (exercising the normal DNS path) and the third is a raw IP, so
    // if the network's DNS is broken the clock still has a chance to sync — the
    // difference between "the clock is wrong" and "the clock works".
    configTime(NTP_TZ_OFFSET_S, NTP_TZ_DST_S,
               NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
}

bool timeReady() {
    if (s_timeReady) return true;
    if (!s_timeArmed) return false;

    struct tm t = {};
    if (getLocalTime(&t, 0)) {
        // Sanity-check the year. An unset clock reads 1970, and a bad reply can
        // land anywhere; anything outside this window must not be shown as time.
        if (t.tm_year >= (2024 - 1900) && t.tm_year <= (2099 - 1900)) {
            s_timeReady = true;
            Serial.printf("[net] NTP synced after %lu attempt(s): "
                          "%04d-%02d-%02d %02d:%02d:%02d local\n",
                          (unsigned long)s_timeAttempts,
                          t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                          t.tm_hour, t.tm_min, t.tm_sec);
            return true;
        }
    }

    // Progress report once a second while unsynced. Without this, a clock that
    // never syncs is indistinguishable from one whose sync call was never made —
    // exactly the ambiguity that made this take longer to find than it should.
    static uint32_t lastReport = 0;
    if (millis() - lastReport >= 1000) {
        lastReport = millis();
        time_t now = 0;
        time(&now);
        const IPAddress dns = WiFi.dnsIP();
        Serial.printf("[net] NTP waiting: attempts=%lu epoch=%ld dns=%s ip=%s rssi=%d\n",
                      (unsigned long)s_timeAttempts, (long)now,
                      dns.toString().c_str(), s_ip, (int)WiFi.RSSI());
    }
    return false;
}

TimeOfDay localTime() {
    TimeOfDay out = {0, 0, 0};
    struct tm t = {};
    if (getLocalTime(&t, 0)) {
        out.hour   = (uint8_t)t.tm_hour;
        out.minute = (uint8_t)t.tm_min;
        out.second = (uint8_t)t.tm_sec;
    }
    return out;
}

const char *macString() {
    // Cached because the station MAC comes from eFuse and cannot change, and
    // because WiFi.macAddress() allocates a String on every call.
    //
    // BUT: WiFi.macAddress() reads from the Wi-Fi driver, not directly from
    // eFuse, and returns "00:00:00:00:00:00" when the driver has not been
    // started yet. Caching that zero value poisoned this function for the rest
    // of the run (the boot banner calls it before any Wi-Fi mode is set).
    // So: bring the STA interface up first, and never cache an all-zero result.
    static char buf[24] = {0};
    if (buf[0] != '\0') return buf;

    // Starting the STA interface initialises the driver so macAddress() returns
    // the real eFuse MAC. Harmless if Wi-Fi is already in STA mode.
    if (WiFi.getMode() == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_STA);
    }

    const String mac = WiFi.macAddress();

    // Normalise to upper-case "AA:BB:CC:DD:EE:FF". The separators are inserted
    // explicitly rather than trusting the core's format: some builds return bare
    // hex, and routers reject that when pasted.
    char hex[13] = {0};
    int n = 0;
    for (unsigned int i = 0; i < mac.length() && n < 12; i++) {
        const char c = mac[i];
        if (isxdigit((unsigned char)c)) {
            hex[n++] = (char)toupper((unsigned char)c);
        }
    }

    // Reject an all-zero MAC: it means the driver was not ready. Returning the
    // placeholder without caching lets a later call report the real address.
    bool all_zero = (n == 12);
    for (int i = 0; i < n; i++) {
        if (hex[i] != '0') { all_zero = false; break; }
    }

    if (n == 12 && !all_zero) {
        snprintf(buf, sizeof(buf), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
                 hex[0], hex[1], hex[2],  hex[3],  hex[4],  hex[5],
                 hex[6], hex[7], hex[8],  hex[9],  hex[10], hex[11]);
        return buf;   // cache only a real address
    }

    // Driver not ready (or an unexpected format). Report something honest and
    // do NOT cache it, so the next call can succeed.
    return "<mac pending>";
}

void disconnect() {
    WiFi.disconnect(true, true);
    s_active = false;
    s_got_ip = false;
    s_choice = NetChoice::NONE;
    strncpy(s_ip, "0.0.0.0", sizeof(s_ip));
    rebuildLine();
}

}  // namespace net
