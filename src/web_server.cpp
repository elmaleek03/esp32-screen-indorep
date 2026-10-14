// web_server.cpp — HTTP API for the Go backend. See web_server.h for the
// threading model.
//
// The two things that matter for reliability here:
//
//  1. /update MUST answer within 5 s. The handler does no drawing, no encoding
//     and no blocking work — it parses, copies, and returns. Everything costly
//     (QR encoding, panel refresh) happens later in the main loop.
//
//  2. The response is sent BEFORE the UI is told anything, so a slow render can
//     never push the reply past the backend's timeout.
#include "web_server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <string.h>

#include "board_config.h"
#include "net.h"
#include "touch_driver.h"

namespace web {

namespace {

WebServer s_server(WEB_SERVER_PORT);
bool      s_running = false;
uint32_t  s_requests = 0;

// --- Mailbox ---------------------------------------------------------------
// Single-slot, last-write-wins. `s_has_payload` is the only cross-context flag.
// Everything here executes in the main loop task (pumped by pump()), so plain
// assignment is sufficient; the volatile flag documents the intent for any
// future change that moves the server to its own task.
PaymentPayload s_mailbox = {};
volatile bool  s_has_payload = false;
volatile bool  s_reset_requested = false;

void sendJson(int code, const char *body) {
    s_server.sendHeader("Connection", "close");
    s_server.send(code, "application/json", body);
}

// POST /update
//
// THIS HANDLER MUST NEVER DO DISPLAY WORK. The Go backend allows 5 s and treats
// a miss as a failed payment, so the reply is sent BEFORE anything is drawn:
// this parses, copies into a mailbox, and returns. The main loop notices the
// mailbox and does the QR encode + panel refresh itself, after the 200 has
// already gone out.
//
// (The old unit on .254 did the rendering inline here, which is why its POST
// averaged ~250 ms and could exceed 5 s under load. That is the regression this
// rewrite exists to prevent — do not reintroduce drawing in this function.)
void handleUpdate() {
    s_requests++;

    if (!s_server.hasArg("plain")) {
        Serial.println("[web] /update: no body");
        sendJson(400, "{\"ok\":false,\"error\":\"missing body\"}");
        return;
    }

    const String &body = s_server.arg("plain");

    // The payload is small (a QRIS string plus a few cart lines), so a fixed
    // document is the right call: no heap churn per request.
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[web] /update: JSON parse failed: %s\n", err.c_str());
        sendJson(400, "{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }

    // Build into a local first, then publish in one step so a half-filled
    // payload can never be claimed by the main loop.
    PaymentPayload p = {};

    const char *qr = doc["qr"] | "";
    if (qr[0] != '\0') {
        snprintf(p.qr, sizeof(p.qr), "%s", qr);
        p.has_qr = true;
    }

    const char *total = doc["total_price"] | "";
    snprintf(p.total, sizeof(p.total), "%s", total);

    JsonArray cart = doc["cart"].as<JsonArray>();
    for (JsonObject item : cart) {
        if (p.item_count >= MAX_DISPLAY_ITEMS) {
            Serial.printf("[web] /update: cart truncated at %d items\n",
                          MAX_DISPLAY_ITEMS);
            break;
        }
        const char *name = item["item"] | "";
        const int qty = item["qty"] | 0;
        // Per-unit price. Optional: an older POS that does not send it leaves
        // this 0 and the UI shows the quantity only (see showQris()).
        const int price = item["price"] | 0;
        if (name[0] == '\0') continue;
        snprintf(p.items[p.item_count].item,
                 sizeof(p.items[p.item_count].item), "%s", name);
        p.items[p.item_count].qty = qty;
        p.items[p.item_count].price = price;
        p.item_count++;
    }

    // Publish. Must happen before the response so a very fast follow-up
    // /update cannot be reordered ahead of this one.
    s_mailbox = p;
    s_has_payload = true;

    // Reply immediately: the drawing happens in the main loop, after this 200
    // has already left the device.
    //
    // The body is FORMAT-COMPATIBLE WITH THE DEVICE THIS REPLACES: the unit that
    // was on 192.168.4.254 replies {"status":"ok"} to both /update and /reset.
    // The Go backend currently only looks at the HTTP status code, but there is
    // no reason to differ — matching it exactly means a future backend change
    // cannot distinguish the two devices.
    sendJson(200, "{\"status\":\"ok\"}");

    // Log AFTER the response, deliberately. Serial.printf at 115200 baud costs
    // ~1 ms per 12 bytes, and a busy USB-UART bridge can block longer than that.
    // Doing it here means console I/O can never delay the reply the backend is
    // waiting on.
    Serial.printf("[web] /update: %u bytes, qr=%u chars cart=%d total=\"%s\"\n",
                  (unsigned)body.length(), (unsigned)strlen(p.qr), p.item_count,
                  p.total);
}

// GET /reset
void handleReset() {
    s_requests++;
    s_reset_requested = true;
    // Same body format as the device being replaced; see handleUpdate().
    sendJson(200, "{\"status\":\"ok\"}");
    Serial.println("[web] /reset");
}

// GET / and /health — a human-checkable status page. The Go backend does not
// call these, but a 200 on / makes "is the device up?" a one-command question.
void handleHealth() {
    s_requests++;
    char body[480];
    // "mac" is the STA hardware address, for a router DHCP reservation.
    //
    // "backend_addr" is where the Go backend is CONFIGURED to knock. It is
    // reported as information, not as a fault: this firmware cannot know whether
    // the backend has been repointed, nor whether another host forwards traffic
    // here. Use it to compare against the device's own "ip" and reason about
    // routing from the backend side.
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"device\":\"indorep-qris-display\","
             "\"fw\":\"%s\",\"mac\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
             "\"backend_addr\":\"%s\",\"same_as_backend\":%s,"
             "\"rssi\":%d,\"touch\":\"%s\",\"requests\":%lu,\"uptime_s\":%lu}",
             "diag-1", net::macString(), net::activeSsid(), net::ipString(),
             NET_BACKEND_ADDR, net::backendUnreachable() ? "false" : "true",
             net::rssi(), touch::available() ? "ok" : "none",
             (unsigned long)s_requests, (unsigned long)(millis() / 1000));
    sendJson(200, body);
}

void handleNotFound() {
    s_requests++;
    // method() returns an http_method enum on this core, not a String.
    Serial.printf("[web] 404 %s %s\n", s_server.method() == HTTP_GET ? "GET" : "OTHER",
                  s_server.uri().c_str());
    sendJson(404, "{\"ok\":false,\"error\":\"not found\"}");
}

}  // namespace

bool begin() {
    if (s_running) return true;

    s_server.on("/update", HTTP_POST, handleUpdate);
    s_server.on("/reset",  HTTP_GET,  handleReset);
    s_server.on("/health", HTTP_GET,  handleHealth);
    s_server.on("/",       HTTP_GET,  handleHealth);
    s_server.onNotFound(handleNotFound);

    // Collect headers so future endpoints can log them; harmless otherwise.
    s_server.enableCORS(true);

    s_server.begin();
    s_running = true;

    Serial.printf("[web] HTTP server listening on %s:%d (ip=%s)\n",
                  net::ipString(), WEB_SERVER_PORT, net::ipString());
    return true;
}

void pump() {
    if (!s_running) return;
    s_server.handleClient();
}

bool running() { return s_running; }

bool hasPending() { return s_has_payload; }

bool takePending(PaymentPayload *out) {
    if (!s_has_payload) return false;
    if (out != nullptr) *out = s_mailbox;
    s_has_payload = false;
    return true;
}

bool takeResetRequest() {
    if (!s_reset_requested) return false;
    s_reset_requested = false;
    return true;
}

uint32_t requestCount() { return s_requests; }

}  // namespace web
