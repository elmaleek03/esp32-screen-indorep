// touch_driver.cpp — GT911 -> LVGL glue. See touch_driver.h for the single-
// reader rule that fixed the loop() freeze.
#include "touch_driver.h"

#include <Arduino.h>
#include <lvgl.h>

#include "board_config.h"
#include "gt911.h"

namespace touch {

namespace {

Stats    s_stats      = {};
bool     s_available  = false;

// Cached state, written only by poll() and read by point()/stats().
bool     s_pressed    = false;
uint16_t s_x          = 0;
uint16_t s_y          = 0;
uint32_t s_last_poll  = 0;
uint32_t s_last_down  = 0;   // millis of the last sample with a finger down

// A panel that reports a sample on every poll but never a release can latch the
// UI "pressed" forever. This is a safety net: if no new sample arrives within
// this window while pressed, treat it as a release.
constexpr uint32_t kStalePressMs = 700;

// When set (via the 'c' console command), every raw touch sample is logged with
// its native coordinates BEFORE mapping. This is the calibration tool: touch
// each corner of the panel and compare the reported raw pair against the
// expected native range.
bool s_traceRaw = false;

// --- Runtime axis transform ------------------------------------------------
//
// Bit flags, so the transform is a data value that can be cycled at runtime
// instead of a set of #if branches that need a reflash to change.
constexpr uint8_t kAxisFlipX = 0x01;
constexpr uint8_t kAxisFlipY = 0x02;
constexpr uint8_t kAxisSwap  = 0x04;

// Start from the compile-time defaults in board_config.h.
constexpr uint8_t kAxisModeDefault =
    (TOUCH_FLIP_X ? kAxisFlipX : 0) |
    (TOUCH_FLIP_Y ? kAxisFlipY : 0) |
    (TOUCH_SWAP_XY ? kAxisSwap : 0);

uint8_t s_axisMode = kAxisModeDefault;

// --- Coordinate mapping ----------------------------------------------------
//
// A small, linear pipeline with no double negatives. Each step says exactly what
// it does, and each is switchable from board_config.h:
//
//   raw controller values
//     -> optional transpose (panel mounted 90 degrees off)
//     -> optional mirror per axis (digitizer upside-down relative to the glass)
//     -> linear scale from the digitizer's raw range onto the panel's pixels
//     -> clamp
//
// The previous implementation reproduced the vendor demo's Arduino map() call
// with its inverted endpoints, then conditionally undid that. Two negations in a
// row is how "touch the top-right button by pressing bottom-left" happens: the
// net effect of that call chain is the IDENTITY when the digitizer already
// matches the glass, which is the case on this unit.
//
// Scale with rounding-to-nearest rather than truncation: truncation biases every
// sample one pixel toward the origin, which is felt as "I have to touch slightly
// lower/left to hit the button".
int32_t scaleAxis(int32_t raw, int32_t rawMax, int32_t pxMax) {
    if (rawMax <= 0) return 0;
    const int32_t num = raw * pxMax;
    // (num + rawMax/2) / rawMax is round-to-nearest for positive values.
    return (num + rawMax / 2) / rawMax;
}

// The whole transform, as one function, so the diagnostic trace and the real
// input path can never disagree about what is being applied.
//
// The transform is driven by the RUNTIME axis mode (s_axisMode), which defaults
// to whatever board_config.h specifies but can be cycled live from the
// calibration screen. Doing it this way means the flip can be found by tapping
// rather than by reflashing and guessing.
void applyTransform(uint16_t rawX, uint16_t rawY, uint16_t *outX, uint16_t *outY) {
    const bool swap = (s_axisMode & kAxisSwap) != 0;
    const bool flipX = (s_axisMode & kAxisFlipX) != 0;
    const bool flipY = (s_axisMode & kAxisFlipY) != 0;

    // 1. Transpose if the panel is mounted rotated relative to the digitizer.
    int32_t rx = rawX;
    int32_t ry = rawY;
    if (swap) {
        const int32_t t = rx; rx = ry; ry = t;
    }

    // 2. Mirror per axis. Mirroring happens on the RAW range, so the scale step
    //    below still runs 0..max rather than max..0.
    const int32_t rawMaxX = TOUCH_NATIVE_W - 1;
    const int32_t rawMaxY = TOUCH_NATIVE_H - 1;
    if (flipX) rx = rawMaxX - rx;
    if (flipY) ry = rawMaxY - ry;

    // 3. Clamp into the assumed digitizer range so a bezel-edge sample cannot
    //    scale outside the panel.
    if (rx < 0) rx = 0;
    if (rx > rawMaxX) rx = rawMaxX;
    if (ry < 0) ry = 0;
    if (ry > rawMaxY) ry = rawMaxY;

    // 4. Scale onto panel pixels, rounding to nearest (truncation biases every
    //    sample one pixel toward the origin, felt as "aim slightly off").
    *outX = (uint16_t)scaleAxis(rx, rawMaxX, LCD_WIDTH - 1);
    *outY = (uint16_t)scaleAxis(ry, rawMaxY, LCD_HEIGHT - 1);
}

void applyMapping(const gt911::Point &p, uint16_t *outX, uint16_t *outY) {
    // Raw values, before anything. This is the log that makes calibration
    // possible: touch each corner and read the pair. Enabled with 'c'.
    //
    // The PAIR is what matters: it tells you which transform is needed without
    // any guessing. Touch the panel's TOP-LEFT and read raw=(rx,ry):
    //   (0,0)             -> no transform
    //   (max,0)           -> flip X
    //   (0,max)           -> flip Y
    //   (max,max)         -> flip X and Y   (180 degrees)
    static uint32_t lastLog = 0;
    if (s_traceRaw && (millis() - lastLog > 200)) {
        lastLog = millis();
        uint16_t mx = 0, my = 0;
        // Report the mapped result alongside the raw one, so a wrong transform
        // is visible directly in the log rather than inferred.
        applyTransform(p.x, p.y, &mx, &my);
        Serial.printf("[touch] raw=(%u,%u) -> mapped=(%u,%u)  [expect top-left "
                      "raw~(0,0) mapped~(0,0)]\n",
                      p.x, p.y, mx, my);
    }

    applyTransform(p.x, p.y, outX, outY);
}

// --- LVGL indev ------------------------------------------------------------
uint16_t s_last_x = 0;
uint16_t s_last_y = 0;

void indevRead(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;

    // THE single reader.
    //
    // This callback runs from lv_timer_handler() in the main loop task, so it is
    // the natural place to talk to the controller: LVGL calls it exactly once per
    // input-processing pass, which is exactly the rate at which it needs fresh
    // state. Nothing else polls any more.
    //
    // The previous design also polled from loop() via pollIfDue(). Both ran in
    // the same task so there was no locking bug, but the controller ACKs each
    // sample as it is read, so whichever caller got there first consumed it.
    // Two independent readers therefore STOLE samples from each other, and a
    // press could be delivered to the LVGL callback as "no change", delaying or
    // dropping a tap. One reader removes that whole class of problem.
    poll();

    if (s_pressed) {
        s_last_x = s_x;
        s_last_y = s_y;
        data->point.x = s_x;
        data->point.y = s_y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->point.x = s_last_x;
        data->point.y = s_last_y;
        data->state   = LV_INDEV_STATE_RELEASED;
    }
}

}  // namespace

bool begin() {
    // gt911::begin() starts the bus itself (gt911_i2c::begin()).
    s_available = gt911::begin();

    if (s_available) {
        Serial.printf("[touch] controller ready at 0x%02X, product \"%s\"\n",
                      gt911::address(), gt911::productId());
    } else {
        Serial.println("[touch] controller NOT available; "
                       "touch input disabled but firmware continues");
    }

    static lv_indev_drv_t indev;
    lv_indev_drv_init(&indev);
    indev.type    = LV_INDEV_TYPE_POINTER;
    indev.read_cb = indevRead;
    lv_indev_drv_register(&indev);
    Serial.println("[touch] LVGL indev registered (single-reader rule active)");

    return s_available;
}

bool available() { return s_available; }

bool poll() {
    s_last_poll = millis();
    s_stats.polls++;

    if (!s_available) {
        s_pressed = false;
        return false;
    }

    // Stale-press watchdog, first, so it applies to every poll regardless of what
    // the controller says next. If a finger is reported down but nothing fresh
    // has arrived for kStalePressMs, the controller has stopped reporting (or the
    // release frame was lost). Releasing here means the UI can never latch.
    if (s_pressed && s_last_down != 0 && (millis() - s_last_down) > kStalePressMs) {
        s_pressed = false;
        s_stats.forced_releases++;
    }

    gt911::Point p = {};
    uint8_t status = 0;
    bool busOk = false;
    const uint8_t n = gt911::readPoints(&p, 1, &status, &busOk);
    s_stats.last_status = status;

    if (!busOk) {
        // The transaction failed: a bus problem, not an idle panel. Counting it
        // makes an intermittently broken bus visible instead of being
        // indistinguishable from "no finger".
        s_stats.bus_errors++;
        s_pressed = false;
        return false;
    }

    if (n == 0) {
        // NO NEW SAMPLE. This does NOT mean the finger lifted.
        //
        // The controller produces a sample at its own rate (~100 Hz) and the
        // ready flag is only set when a fresh sample exists, so most polls land
        // between samples. Reporting "released" here made the pressed state flap
        // at poll rate; LVGL then synthesised CLICKED/RELEASED pairs it never
        // should have, and could strand its internal act_obj pointer so that no
        // later press reached any object. That is what made every screen after
        // the first become completely unresponsive.
        //
        // The controller DOES tell us about a lift: it reports a ready frame with
        // count == 0. gt911::readPoints() consumes that and returns 0, but it
        // also reports it via the status byte, which is how the two cases are
        // told apart here.
        const bool releaseFrame = (status & 0x80) && ((status & 0x0F) == 0);
        if (releaseFrame) {
            if (s_pressed) {
                s_stats.releases++;
            }
            s_pressed = false;
        }
        // Otherwise: hold the previous state. No sample is not a release.
        return s_pressed;
    }

    // A sample with a finger down.
    uint16_t mx = 0, my = 0;
    applyMapping(p, &mx, &my);

    s_x = mx;
    s_y = my;
    s_pressed = true;
    s_last_down = millis();
    s_stats.samples++;
    s_stats.last_x = mx;
    s_stats.last_y = my;

    return true;
}

bool pollIfDue(uint32_t interval_ms) {
    // Kept for callers that want a rate-limited read (and for the console 'd'
    // dump). The main loop does NOT use this: the LVGL indev callback is now the
    // only reader, which is what makes sample handling deterministic.
    if (millis() - s_last_poll < interval_ms) return false;
    poll();
    return true;
}

void forceRelease() {
    // Drop the reported press AND forget the cached coordinates.
    //
    // Called on every screen transition. Without it, LVGL's indev keeps its
    // internal act_obj pointing at a widget on the screen we just hid, and the
    // RELEASED event that would clear it is never delivered to the new screen —
    // so the new screen receives no input at all. This is the single most
    // important line for "touch works on the menu but nothing works after
    // navigating".
    const bool was = s_pressed;
    s_pressed = false;
    s_last_down = 0;
    if (was) s_stats.forced_releases++;
}

bool point(uint16_t *x, uint16_t *y) {
    if (!s_pressed) return false;
    if (x) *x = s_x;
    if (y) *y = s_y;
    return true;
}

Stats stats() {
    Stats s = s_stats;
    s.last_x = s_x;
    s.last_y = s_y;
    return s;
}

void logHealth() {
    const Stats s = stats();
    if (!s_available) {
        Serial.println("[touch] health: controller unavailable (no GT911 found)");
        return;
    }
    Serial.printf("[touch] health: polls=%lu samples=%lu releases=%lu "
                  "forced=%lu bus_errors=%lu down=%d x=%u y=%u status=0x%02X "
                  "axis=%s product=%s\n",
                  (unsigned long)s.polls, (unsigned long)s.samples,
                  (unsigned long)s.releases, (unsigned long)s.forced_releases,
                  (unsigned long)s.bus_errors,
                  s_pressed ? 1 : 0, s.last_x, s.last_y, s.last_status,
                  axisModeName(s_axisMode), gt911::productId());
}

void dumpRegisters() {
    gt911::dumpIdentity();
}

void setRawTrace(bool on) {
    s_traceRaw = on;
    Serial.printf("[touch] raw coordinate trace %s\n", on ? "ON" : "OFF");
    if (on) {
        Serial.printf("[touch]   driver assumes native range X:0..%d Y:0..%d\n",
                      TOUCH_NATIVE_W, TOUCH_NATIVE_H);
        Serial.println("[touch]   touch each panel corner; expected raw pairs:");
        Serial.printf("[touch]     top-left     ~(0,0)      (mapped 0,0)\n");
        Serial.printf("[touch]     top-right    ~(%d,0)    (mapped %d,0)\n",
                      TOUCH_NATIVE_W, LCD_WIDTH - 1);
        Serial.printf("[touch]     bottom-left  ~(0,%d)    (mapped 0,%d)\n",
                      TOUCH_NATIVE_H, LCD_HEIGHT - 1);
        Serial.printf("[touch]     bottom-right ~(%d,%d)  (mapped %d,%d)\n",
                      TOUCH_NATIVE_W, TOUCH_NATIVE_H, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    }
}

bool rawTrace() { return s_traceRaw; }

// --- Axis calibration ------------------------------------------------------

uint8_t axisModeCount() { return 8; }

const char *axisModeName(uint8_t mode) {
    switch (mode & 0x07) {
        case 0: return "normal";
        case 1: return "flip X";
        case 2: return "flip Y";
        case 3: return "flip X+Y (180)";
        case 4: return "swap XY";
        case 5: return "swap + flip X";
        case 6: return "swap + flip Y";
        case 7: return "swap + flip X+Y";
        default: return "?";
    }
}

void setAxisMode(uint8_t mode) {
    // 0..7 are the full set of meaningful combinations.
    s_axisMode = (uint8_t)(mode & 0x07);
    Serial.printf("[touch] axis transform -> %s (raw mode %u)\n",
                  axisModeName(s_axisMode), s_axisMode);
    Serial.println("[touch]   verify by touching the panel's top-left corner;");
    Serial.println("[touch]   the reported mapped point should be near (0,0).");
}

uint8_t axisMode() { return s_axisMode; }

}  // namespace touch
