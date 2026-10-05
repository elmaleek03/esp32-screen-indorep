// main.cpp — Indorep QRIS mini display firmware.
//
// This is a complete rewrite of the M0 bring-up firmware. It implements the
// whole product flow in docs/PLAN.md:
//
//   BOOT_MENU -> CONNECTING -> CONFIRM -> HOME  <- (POST /update) <- QRIS
//                                          ^                        |
//                                          +----- GET /reset -------+
//
// Four architectural rules, each of which exists because breaking it caused a
// specific, observed failure in the previous revision:
//
//  1. ONE I2C READER. touch::poll() is the only function that talks to the
//     GT911. The old build had three readers (LVGL indev, a main-loop readout,
//     and a 500 ms register dump); concurrent transactions held SCL low and
//     froze loop() permanently.
//
//  2. ONE LVGL TASK. Every widget access happens here, in loop(). The web
//     server publishes a payload into a mailbox and returns; this loop consumes
//     it and performs the transition between two lv_timer_handler calls.
//
//  3. NO BLOCKING FLUSH. display::tick() waits for the panel's DMA-done flag in
//     task context, then releases LVGL. The old build released LVGL immediately
//     from the flush callback, which let LVGL rewrite the buffer the panel was
//     still reading.
//
//  4. DIAGNOSTICS ARE ALWAYS ON. A heartbeat every DIAG_HEARTBEAT_MS plus a
//     serial command console. The previous freeze was hard to diagnose because
//     the log was silent exactly when it mattered.
//
// Serial console (open the monitor and press a key):
//   h / ?   help
//   d       dump GT911 registers + panel/touch/i2c statistics
//   s       rescan the touch I2C bus
//   t       toggle per-touch-sample logging
//   r       reset the UI to the standby homescreen (same as GET /reset)
//   q       re-encode a test QRIS payload locally (UI smoke test, no network)
//   i       print a full system report
#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include "app_state.h"
#include "board_config.h"
#include "display_driver.h"
#include "gt911.h"
#include "gt911_i2c.h"
#include "net.h"
#include "qr_render.h"
#include "touch_driver.h"
#include "ui_screens.h"
#include "web_server.h"
#include "wifi_profiles.h"

// ---------------------------------------------------------------------------
// Deferred actions raised by the UI and consumed by this loop.
//
// The UI callbacks cannot call net::beginConnect() directly and then
// immediately switch screens, because the transition must happen at a
// well-defined point in the loop (never from inside LVGL's event dispatch,
// which runs nested inside lv_timer_handler).
// ---------------------------------------------------------------------------
namespace {

enum class Action : uint8_t {
    NONE = 0,
    CONNECT,
    BACK_TO_MENU,
};

Action   s_action       = Action::NONE;
NetChoice s_actionChoice = NetChoice::NONE;
bool     s_traceTouches = false;

uint32_t s_bootMs = 0;

// --- serial console --------------------------------------------------------
void printHelp() {
    Serial.println();
    Serial.println("--- serial console ------------------------------------------");
    Serial.println("  h / ?  this help");
    Serial.println("  d      dump GT911 registers + panel/touch/i2c statistics");
    Serial.println("  s      rescan the touch I2C bus");
    Serial.println("  t      toggle per-touch-sample logging");
    Serial.println("  c      toggle RAW touch coordinate trace (calibration)");
    Serial.println("  x      cycle touch axis transform (normal/flip/swap, 8 modes)");
    Serial.println("  b      cycle backlight 100/75/50/35/20/10/5/0%");
    Serial.println("  = / -  backlight +5% / -5%");
    Serial.println("  w      scan Wi-Fi now (blocks ~2-4 s; not done at boot)");
    Serial.println("  r      return the UI to the standby homescreen");
    Serial.println("  q      show a local test QRIS page (no network needed)");
    Serial.println("  i      full system report");
    Serial.println("  m      print the station MAC (for a router static lease)");
    Serial.println("-------------------------------------------------------------");
}

void printSystemReport() {
    const uint32_t up = (millis() - s_bootMs) / 1000;
    Serial.println();
    Serial.println("=== SYSTEM REPORT ===========================================");
    Serial.printf(" uptime      : %luh %lum %lus\n",
                  (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
                  (unsigned long)(up % 60));
    Serial.printf(" cpu         : %u MHz, heap free %u KB (min %u KB), "
                  "psram free %u KB\n",
                  (unsigned)getCpuFrequencyMhz(),
                  (unsigned)(ESP.getFreeHeap() / 1024),
                  (unsigned)(ESP.getMinFreeHeap() / 1024),
                  (unsigned)(ESP.getFreePsram() / 1024));
    Serial.printf(" lcd         : %dx%d, backlight GPIO%d at %u%%\n",
                  LCD_WIDTH, LCD_HEIGHT, PIN_LCD_BL,
                  (unsigned)display::brightness());
    Serial.printf(" touch       : %s @0x%02X product \"%s\" (sda=%d scl=%d rst=%d)\n",
                  touch::available() ? "GT911 OK" : "NOT DETECTED",
                  gt911::address(), gt911::productId(),
                  PIN_TOUCH_SDA, PIN_TOUCH_SCL, PIN_TOUCH_RST);
    Serial.printf(" i2c         : freq=%u Hz\n", (unsigned)TOUCH_I2C_FREQ);
    Serial.printf(" wifi        : %s\n", net::statusLine());
    Serial.printf(" ip          : %s  rssi=%d dBm\n", net::ipString(), net::rssi());
    Serial.printf(" mac (STA)   : %s   <- router DHCP reservation\n",
                  net::macString());
    Serial.printf(" backend tgt : %s  %s\n", NET_BACKEND_ADDR,
                  net::backendUnreachable()
                      ? "(differs from this device's address)"
                      : "(this device's address)");
    Serial.printf(" web server  : %s, %lu request(s)\n",
                  web::running() ? "RUNNING :80" : "stopped",
                  (unsigned long)web::requestCount());
    Serial.printf(" screen      : %d\n", (int)ui::current());
    Serial.println("=============================================================");
}

void serviceSerialConsole() {
    if (!Serial.available()) return;

    const int c = Serial.read();
    // Drain anything else so a paste does not queue hundreds of commands.
    while (Serial.available()) Serial.read();

    switch (c) {
        case 'h': case 'H': case '?':
            printHelp();
            break;
        case 'd': case 'D':
            Serial.println("[diag] register dump requested");
            touch::dumpRegisters();
            display::logHealth();
            touch::logHealth();
            break;
        case 's': case 'S':
            Serial.println("[diag] rescanning touch I2C bus...");
            gt911_i2c::scan();
            break;
        case 't': case 'T':
            s_traceTouches = !s_traceTouches;
            Serial.printf("[diag] touch trace %s\n", s_traceTouches ? "ON" : "OFF");
            break;
        case 'c': case 'C':
            touch::setRawTrace(!touch::rawTrace());
            break;
        case 'x': case 'X': {
            // Cycle the touch axis transform without opening the calibration
            // page. Prints the new name so the log records what was tried.
            const uint8_t next =
                (uint8_t)((touch::axisMode() + 1) % touch::axisModeCount());
            touch::setAxisMode(next);
            break;
        }
        case 'b': case 'B': {
            // Cycle the backlight through fixed steps. A live control is worth
            // having because "comfortable brightness" is subjective: check it on
            // the actual counter rather than guessing a number and reflashing.
            static const uint8_t kSteps[] = {100, 75, 50, 35, 20, 10, 5, 0};
            static uint8_t idx = 0;
            idx = (uint8_t)((idx + 1) % (sizeof(kSteps) / sizeof(kSteps[0])));
            display::setBrightness(kSteps[idx]);
            Serial.printf("[diag] backlight -> %u%%\n", (unsigned)kSteps[idx]);
            break;
        }
        case '=':
            // Fine-grained adjust, +5% per press, clamped at 100.
            display::setBrightness((uint8_t)min<uint16_t>(100, display::brightness() + 5));
            Serial.printf("[diag] backlight -> %u%%\n", (unsigned)display::brightness());
            break;
        case '-':
            // -5% per press, clamped at 0.
            display::setBrightness((uint8_t)(display::brightness() >= 5
                                                 ? display::brightness() - 5
                                                 : 0));
            Serial.printf("[diag] backlight -> %u%%\n", (unsigned)display::brightness());
            break;
        case 'w': case 'W': {
            // On-demand Wi-Fi scan. This BLOCKS for 2-4 seconds, which is why it
            // is not done at boot any more: blocking here delays display::tick()
            // and therefore input. On a console command that is expected; at boot
            // it was the "unresponsive for a few seconds" symptom.
            Serial.println("[diag] scanning Wi-Fi (this blocks ~2-4 s)...");
            const int n = WiFi.scanNetworks();
            Serial.printf("[diag] %d network(s):\n", n);
            for (int i = 0; i < n; i++) {
                Serial.printf("[diag]   %-30s rssi=%4d ch=%2d enc=%d\n",
                              WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                              WiFi.channel(i), (int)WiFi.encryptionType(i));
            }
            break;
        }
        case 'r': case 'R':
            Serial.println("[diag] reset -> standby");
            ui::showHome();
            ui::setHomeStatus("menunggu transaksi...");
            break;
        case 'q': case 'Q': {
            Serial.println("[diag] local test QRIS page");
            static PaymentPayload demo = {};
            snprintf(demo.qr, sizeof(demo.qr),
                     "00020101021226610014ID.CO.QRIS.WWW011893600"
                     "9140000000000000215041234567890123450303UMI"
                     "51440014ID.CO.QRIS.WWW0215ID10200212345670303U"
                     "MI5204581253033605802ID5920TOKO TEST INDOREP6007JAKARTA"
                     "61051234062070703A016304ABCD");
            snprintf(demo.total, sizeof(demo.total), "Rp 15 000");
            demo.item_count = 3;
            snprintf(demo.items[0].item, sizeof(demo.items[0].item), "Kopi Susu");
            demo.items[0].qty = 1;
            snprintf(demo.items[1].item, sizeof(demo.items[1].item), "Paket Malam");
            demo.items[1].qty = 1;
            snprintf(demo.items[2].item, sizeof(demo.items[2].item), "Roti Bakar");
            demo.items[2].qty = 2;
            demo.has_qr = true;
            ui::showQris(demo);
            break;
        }
        case 'i': case 'I':
            printSystemReport();
            break;
        case 'm': case 'M':
            // Print just the MAC, on its own line, so it can be copied straight
            // out of the terminal into a router's DHCP reservation.
            Serial.printf("[diag] STA MAC = %s\n", net::macString());
            break;
        default:
            Serial.printf("[diag] unknown command '%c' (press h for help)\n", c);
            break;
    }
}

// --- diagnostics -----------------------------------------------------------
uint32_t s_touchSamplesLast = 0;

void logHeartbeat() {
    static uint32_t last = 0;
    static display::Stats prev = {};
    if (millis() - last < DIAG_HEARTBEAT_MS) return;
    last = millis();

    const uint32_t up = millis() / 1000;
    const display::Stats ds = display::stats();
    const touch::Stats  ts = touch::stats();

    // Per-interval deltas, not totals: the question "is the screen being
    // redrawn right now, and with what" is only answerable from a delta.
    const uint32_t d_full = ds.full_flushes - prev.full_flushes;
    const uint32_t d_part = ds.partial_flushes - prev.partial_flushes;
    prev = ds;

    Serial.printf("[hb] t=%lus screen=%d wifi=%s web=%s "
                  "flush+%lu(full %lu part %lu) "
                  "touch=%lu/%lu(rel %lu forced %lu) heap=%uK psram=%uK\n",
                  (unsigned long)up, (int)ui::current(), net::statusLine(),
                  web::running() ? "up" : "down",
                  (unsigned long)(d_full + d_part), (unsigned long)d_full,
                  (unsigned long)d_part,
                  (unsigned long)ts.samples, (unsigned long)ts.polls,
                  (unsigned long)ts.releases, (unsigned long)ts.forced_releases,
                  (unsigned)(ESP.getFreeHeap() / 1024),
                  (unsigned)(ESP.getFreePsram() / 1024));
}

void logTouchTrace() {
    if (!s_traceTouches) return;

    // Only log when a sample actually arrived, so the trace is readable and
    // does not itself become the noise that hid the bug last time.
    const touch::Stats st = touch::stats();
    if (st.samples == s_touchSamplesLast) return;
    s_touchSamplesLast = st.samples;

    uint16_t x = 0, y = 0;
    if (touch::point(&x, &y)) {
        Serial.printf("[touch] sample #%lu x=%u y=%u status=0x%02X\n",
                      (unsigned long)st.samples, x, y, st.last_status);
    } else {
        Serial.printf("[touch] release (polls=%lu)\n", (unsigned long)st.polls);
    }
}

// --- deferred actions ------------------------------------------------------
void runPendingActions() {
    switch (s_action) {
        case Action::CONNECT: {
            const NetChoice c = s_actionChoice;
            s_action = Action::NONE;
            ui::showConnecting(net::ssidFor(c));
            net::beginConnect(c);
            break;
        }
        case Action::BACK_TO_MENU:
            s_action = Action::NONE;
            // Abort an in-flight association if there is one (the "Batal" button
            // on the connecting screen). If we are already connected, leave the
            // link alone: somebody backing out of the menu should not knock the
            // device off the network it is serving the backend on.
            if (net::connecting()) {
                Serial.println("[app] cancel: aborting association");
                net::disconnect();
            }
            ui::showBootMenu();
            break;
        case Action::NONE:
        default:
            break;
    }
}

// --- wifi / server lifecycle ----------------------------------------------
bool s_serverStarted = false;

void serviceNetwork() {
    if (!net::connecting()) return;

    if (net::poll()) {
        // Connected: bring the web server up and move to CONFIRM.
        const bool ok = web::begin();
        s_serverStarted = ok;
        ui::showConfirm(net::activeSsid(), net::ipString(), ok);
        Serial.printf("[boot] web server %s\n", ok ? "RUNNING" : "FAILED");
        return;
    }

    if (net::failed()) {
        // showConnectFailed() records the reason and switches back to the menu,
        // so the operator sees WHY rather than watching the menu silently
        // reappear (which looks like the tap was never registered).
        Serial.println("[boot] connect failed, returning to menu");
        ui::showConnectFailed(net::activeSsid(), net::failureReason());
        return;
    }

    // Still connecting: refresh the status line AT MOST TWICE A SECOND.
    //
    // This used to run on every loop pass (~500 Hz). Every call reached
    // lv_label_set_text(), which marks the label dirty and makes LVGL redraw it
    // on the next timer run — a permanent redraw storm. That storm is what made
    // the connecting screen ignore touch (the UI never settled between frames,
    // so a press/release pair could be swallowed) and kept the panel visibly
    // busy. Rate-limiting it is not an optimisation; it is the fix.
    static uint32_t lastStatusMs = 0;
    if (millis() - lastStatusMs < 500) return;
    lastStatusMs = millis();

    ui::setConnectingStatus(net::statusLine());
}

// --- web payload consumption (main-loop task only) -------------------------
void serviceWebPayload() {
    PaymentPayload p = {};
    if (web::takePending(&p)) {
        Serial.printf("[app] rendering payload: qr=%s cart=%d total=\"%s\"\n",
                      p.has_qr ? "yes" : "none", p.item_count, p.total);
        ui::showQris(p);
    }

    if (web::takeResetRequest()) {
        // Unconditional: /reset means "go back to standby" and must work from
        // ANY state. The Go backend calls it at the end of a payment without
        // knowing what the device is displaying, so refusing it when the device
        // happens to already be on standby would leave stale items on screen
        // after the next /update. Idempotent by design.
        Serial.printf("[app] /reset -> standby (was screen %d)\n",
                      (int)ui::current());
        ui::showHome();
        ui::setHomeStatus("menunggu transaksi...");
    }
}

// --- homescreen status -----------------------------------------------------
void serviceHomeStatus() {
    if (ui::current() != Screen::HOME) return;

    static uint32_t last = 0;
    if (millis() - last < 3000) return;
    last = millis();

    // Plain device status. Deliberately says nothing about the backend: the
    // firmware cannot know where the backend points or whether traffic reaches
    // it, so claiming "UNREACHABLE" here would be a guess presented as a fault.
    // What it CAN state is its own address, which is what an operator needs to
    // check routing against.
    char buf[128];
    snprintf(buf, sizeof(buf), "%s  |  ip %s  rssi %d dBm  |  :80 %s",
             net::activeSsid(), net::ipString(), net::rssi(),
             web::running() ? "READY" : "DOWN");
    ui::setHomeStatus(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points called by the UI (declared inside ui_screens.cpp).
// They only record intent; the loop performs the transition.
// ---------------------------------------------------------------------------
void uiRequestConnect(NetChoice choice) {
    s_action = Action::CONNECT;
    s_actionChoice = choice;
}

void uiRequestBack() {
    s_action = Action::BACK_TO_MENU;
}

// ---------------------------------------------------------------------------
void setup() {
    s_bootMs = millis();

    Serial.begin(115200);
    delay(700);   // let the USB-UART bridge settle before the banner

    Serial.println();
    Serial.println("=============================================================");
    Serial.println(" INDOREP QRIS MINI DISPLAY - DIAGNOSTIC FIRMWARE (full rewrite)");
    Serial.println(" board: ESP32-4827S043  4.3in 480x272 ILI6485, GT911 touch");
    Serial.println("=============================================================");
    Serial.printf(" chip   : %s rev%d, %d core(s) @ %u MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                  (unsigned)getCpuFrequencyMhz());
    Serial.printf(" flash  : %u KB\n", (unsigned)(ESP.getFlashChipSize() / 1024));
    Serial.printf(" psram  : %u KB free / %u KB total\n",
                  (unsigned)(ESP.getFreePsram() / 1024),
                  (unsigned)(ESP.getPsramSize() / 1024));
    Serial.printf(" heap   : %u KB free\n", (unsigned)(ESP.getFreeHeap() / 1024));
    if (ESP.getPsramSize() == 0) {
        Serial.println(" [FATAL] no PSRAM detected! check board_build.arduino.memory_type");
    }

    // --- 1. LVGL + panel ---------------------------------------------------
    lv_init();
    if (!display::begin()) {
        Serial.println("[boot] FATAL: display init failed, halting");
        while (true) delay(1000);
    }

    // --- 2. Panel self-test, BEFORE LVGL draws anything --------------------
    // Turn the backlight on early for this one step so the colour-bar pattern is
    // actually visible. If it shows up, the RGB bus, pins, timings and polarity
    // are all proven correct and any later blank screen is an LVGL or flush
    // problem — which is exactly the ambiguity that made the last attempt hard
    // to read. It also gives the panel a known non-white frame immediately.
    // Turn the backlight on at the configured level so the colour-bar pattern is
    // actually visible. LCD_BL_START_PERCENT defaults to 50%: full brightness is
    // harsh for a counter-facing panel and is the largest power/heat consumer in
    // the device. See board_config.h.
    display::setBrightness(LCD_BL_START_PERCENT);
    Serial.printf("[boot] backlight on at %u%%\n",
                  (unsigned)display::brightness());
    if (!display::selfTest()) {
        Serial.println("[boot] WARNING: panel self-test could not run");
    }

    // --- 3. Touch ----------------------------------------------------------
    const bool touchOk = touch::begin();
    Serial.printf("[boot] touch init: %s\n", touchOk ? "OK" : "UNAVAILABLE");

    // --- 4. UI -------------------------------------------------------------
    ui::build();
    Serial.println("[boot] UI built (boot menu)");

    // The self-test left its pattern on the glass. Force LVGL to redraw the
    // whole screen so the boot menu replaces it rather than being composited on
    // top of stale test pixels. A few ticks are needed because the flush is
    // completed in task context by display::tick() itself.
    lv_obj_invalidate(lv_scr_act());
    for (int i = 0; i < 3; i++) {
        display::tick();
        delay(20);
    }
    Serial.printf("[boot] first LVGL frame pushed (flushes=%lu)\n",
                  (unsigned long)display::stats().flushes);

    // --- 5. Wi-Fi: mode only -------------------------------------------------
    //
    // NO SCAN HERE. WiFi.scanNetworks() blocks for 2-4 seconds, and while it
    // blocks, display::tick() never runs, so the UI is frozen — which is exactly
    // the "unresponsive for a few seconds at boot" that was observed.
    //
    // The scan result was only ever informational (it listed APs and reported
    // which menu SSIDs were visible). Losing that at boot costs nothing: the
    // operator picks an AP from a fixed list, and a failure is reported with an
    // actionable reason on the menu. A scan is still available on demand with
    // the 'w' console command, where blocking is expected and harmless.
    //
    // Do NOT call WiFi.disconnect() straight after mode(WIFI_STA) either: on
    // this core mode() only records the requested mode, it does not start the
    // STA driver, so the disconnect would log "STA not started!" and do nothing.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);   // keep the HTTP server responsive while drawing

    // Print the station MAC. This is a hardware constant from the eFuse, valid
    // before any association, and it is what you paste into a router's DHCP
    // reservation (e.g. a MikroTik "static lease"). Printed here, shown on the
    // CONFIRM screen, and exposed on /health, so it can be read without a serial
    // console if the device is already installed.
    Serial.printf("[net] STA MAC : %s\n", net::macString());
    Serial.println("[net]           ^ use this for the router's DHCP reservation");

    Serial.println();
    Serial.println("[boot] setup complete - running (Wi-Fi idle until an AP is chosen)");
    printHelp();
    Serial.println();
}

void loop() {
    // ---------------------------------------------------------------------
    // ORDERING CONTRACT
    //
    // display::tick() is the thing that keeps the UI alive: it completes the
    // panel flush, runs the LVGL timers, and (through the indev callback) reads
    // touch. Anything that can BLOCK must therefore not sit in front of it in a
    // way that starves it.
    //
    // That is why the steps below are ordered as they are, and why the Wi-Fi
    // accessors are rate-limited inside net::poll() (each one takes the ESP-IDF
    // Wi-Fi driver mutex, which the Wi-Fi task holds for long stretches during
    // association — calling them per loop pass is what made the connecting
    // screen stop accepting touch).
    // ---------------------------------------------------------------------

    // 1. Draw + process input FIRST, every iteration, unconditionally.
    //    Everything else in this loop is optional work that must not delay it.
    display::tick();

    // 2. Serial console (non-blocking).
    serviceSerialConsole();

    // 3. Network progress (may raise a screen change via ui::*). Rate-limited
    //    internally to 10 Hz so it cannot hold the loop open.
    serviceNetwork();

    // 4. HTTP: serve a request, publish any payload. Never touches LVGL.
    web::pump();

    // 5. Consume deferred UI actions and web payloads.
    runPendingActions();
    serviceWebPayload();

    // 6. Time-based UI refresh + always-on diagnostics.
    ui::tick();
    serviceHomeStatus();
    logTouchTrace();
    logHeartbeat();

    // Yield to the idle task and the Wi-Fi/LWIP stack. 1 tick is enough; a
    // larger value directly adds input latency, because display::tick() only
    // runs once per loop pass.
    delay(1);
}
