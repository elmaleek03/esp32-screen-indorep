// net.cpp — Wi-Fi station bring-up for WARNET / CAFE / IDRP(debug).
//
// Behaviour notes:
//  * The Go backend hardcodes http://192.168.4.254/, so when NET_STATIC_IP is
//    enabled this applies a static address BEFORE begin(). Configuring after
//    begin() is too late: DHCP would already have assigned something else and
//    the backend's first request would time out.
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

// Effective IP policy for the current attempt (from NET_IP_MODE).
int  s_ipMode      = NET_IP_MODE;
// True once the static fallback has been applied for this attempt, so the DHCP
// evaluation in poll() cannot oscillate between DHCP and static on every pass.
bool s_usedFallback = false;

// Claim NET_FALLBACK_ADDR statically. Returns true if the address was accepted.
//
// Called in two places: before begin() in static-only mode, and from poll() in
// mode 1 once DHCP has returned an address that is NOT the one the backend
// needs. Doing it late is the whole point of the fallback — DHCP gets first say,
// so a working reservation is preferred over a hard-coded claim.
bool applyStaticAddress() {
    IPAddress ip, gw, mask;
    if (!ip.fromString(NET_FALLBACK_ADDR) || !gw.fromString(NET_FALLBACK_GW) ||
        !mask.fromString(NET_FALLBACK_MASK)) {
        Serial.println("[net] ERROR: NET_FALLBACK_ADDR/GW/MASK did not parse!");
        return false;
    }

    // A DNS server IS required, even though this device is primarily a server.
    // The standby clock resolves its NTP servers by hostname, so without DNS the
    // sync fails and the clock never updates - silently. NET_DNS_SERVER defaults
    // to the gateway, which normally runs a resolver.
    IPAddress dns;
    const bool haveDns = dns.fromString(NET_DNS_SERVER);
    if (!haveDns) {
        Serial.printf("[net] WARNING: NET_DNS_SERVER \"%s\" did not parse; "
                      "NTP hostname lookups will fail\n", NET_DNS_SERVER);
    }

    // WiFi.config() has several overloads; the 4-arg form adds a DNS server.
    const bool ok = haveDns ? WiFi.config(ip, gw, mask, dns)
                            : WiFi.config(ip, gw, mask);
    if (!ok) {
        Serial.println("[net] ERROR: static address REJECTED by the driver");
        Serial.printf("[net]   requested %s/%s gw %s\n",
                      NET_FALLBACK_ADDR, NET_FALLBACK_MASK, NET_FALLBACK_GW);
        return false;
    }

    snprintf(s_ip, sizeof(s_ip), "%s", NET_FALLBACK_ADDR);
    s_usedFallback = true;

    Serial.printf("[net] static address claimed: %s gw %s mask %s dns %s\n",
                  NET_FALLBACK_ADDR, NET_FALLBACK_GW, NET_FALLBACK_MASK,
                  haveDns ? NET_DNS_SERVER : "(none)");

    // Say plainly whether the backend can reach us, because this is the single
    // most likely reason a correctly-installed device appears not to work.
    if (strcmp(NET_FALLBACK_ADDR, NET_BACKEND_ADDR) == 0) {
        Serial.println("[net]   backend target matches this address: OK");
    } else {
        Serial.printf("[net]   WARNING: the backend calls %s, this device is %s\n",
                      NET_BACKEND_ADDR, NET_FALLBACK_ADDR);
        Serial.println("[net]   the two must be bridged (e.g. a MikroTik dst-nat "
                       "rule .254:80 -> this address) or the backend will time out");
    }
    return true;
}

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
    s_usedFallback = false;   // each attempt re-evaluates DHCP before falling back
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

    s_ipMode = NET_IP_MODE;

#if NET_IP_MODE == 2
    // STATIC ONLY: claim the address before begin(), so DHCP never runs.
    applyStaticAddress();
#else
    // DHCP (modes 0 and 1). In mode 1 a static fallback is applied later, from
    // poll(), but only after DHCP has had its say — see the transition there.
    Serial.printf("[net] DHCP mode%s\n",
                  (NET_IP_MODE == 1) ? " with static fallback" : " (no fallback)");
    Serial.printf("[net]   want %s for MAC %s\n", NET_BACKEND_ADDR, macString());
#endif

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

        // --- Mode 1: DHCP got us an address. Is it the one the backend needs? --
        //
        // This is the DHCP-then-static-fallback decision, and it happens HERE
        // because it can only be made once DHCP has answered. If the lease is
        // wrong, the device switches to the static fallback rather than sitting
        // on an address nothing will ever talk to.
        if (s_ipMode == 1 && !s_usedFallback) {
            if (strcmp(s_ip, NET_BACKEND_ADDR) != 0) {
                Serial.printf("[net] DHCP gave %s, but the backend calls %s\n",
                              s_ip, NET_BACKEND_ADDR);
                Serial.println("[net]   switching to the static fallback address");

                // Drop the lease first: leaving a DHCP address configured while
                // adding a static one is a guaranteed address conflict.
                WiFi.disconnect(true, true);
                delay(50);
                if (applyStaticAddress()) {
                    // Re-associate so the new address takes effect.
                    //
                    // Restart the timeout clock: this is a second association
                    // attempt, and keeping the original deadline would make it
                    // expire almost immediately after a slow first DHCP round.
                    s_started = millis();
                    WiFi.begin(ssidFor(s_choice),
                               passFor(s_choice)[0] ? passFor(s_choice) : "");
                    return false;   // still connecting; poll() will finish it
                }
                Serial.println("[net]   fallback FAILED; staying on the DHCP address");
            } else {
                Serial.printf("[net] DHCP gave the required address %s: no fallback needed\n",
                              s_ip);
            }
        }

        s_got_ip = true;
        s_active = false;
        s_failed = false;
        // Read the descriptive fields only on the transition to connected, not
        // on every poll.
        Serial.printf("[net] connected: ssid=\"%s\" ip=%s rssi=%d dBm ch=%d gw=%s%s\n",
                      WiFi.SSID().c_str(), s_ip, (int)WiFi.RSSI(),
                      (int)WiFi.channel(), WiFi.gatewayIP().toString().c_str(),
                      s_usedFallback ? "  (static fallback)" : "");
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
// configTime() hands the work to the ESP-IDF SNTP client and returns, and
// localTime() just reads the system clock.
//
// The device has NO RTC and no battery, so time is lost on every power cycle.
// That is why the UI needs an explicit "unsynced" state rather than assuming a
// clock is available.
static bool     s_timeSyncStarted = false;
static bool     s_timeReady       = false;
static uint32_t s_timeSyncStart   = 0;

void beginTimeSync() {
    if (s_timeSyncStarted) return;
    s_timeSyncStarted = true;
    s_timeSyncStart = millis();

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
    Serial.printf("[net] NTP sync started (%s, %s, %s), TZ offset %+ld s\n",
                  NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3,
                  (long)NTP_TZ_OFFSET_S);

    // Report the DNS server the stack is actually using. A missing or wrong DNS
    // entry is the single most likely reason the hostname servers fail, so it is
    // worth stating rather than leaving to inference.
    const IPAddress dns = WiFi.dnsIP();
    Serial.printf("[net] DNS for NTP lookups: %s%s\n",
                  dns.toString().c_str(),
                  (dns[0] == 0 && dns[1] == 0) ? "  (NONE SET - hostname NTP will fail)" : "");
}

bool timeReady() {
    if (s_timeReady) return true;
    if (!s_timeSyncStarted) return false;

    struct tm t = {};
    if (getLocalTime(&t, 0)) {
        // Sanity-check the year. An unset clock reads 1970, and a bad reply can
        // land anywhere; anything outside this window must not be shown as time.
        if (t.tm_year >= (2024 - 1900) && t.tm_year <= (2099 - 1900)) {
            s_timeReady = true;
            Serial.printf("[net] NTP synced: %04d-%02d-%02d %02d:%02d:%02d local\n",
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
        Serial.printf("[net] NTP waiting: epoch=%ld dns=%s ip=%s rssi=%d\n",
                      (long)now, dns.toString().c_str(), s_ip, (int)WiFi.RSSI());
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
