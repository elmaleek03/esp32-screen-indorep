// touch_driver.h — GT911 -> LVGL glue, with exactly one I2C reader.
//
// THE RULE THAT FIXED THE FREEZE
//
// The previous build had three code paths reading the GT911: the LVGL indev
// callback, main.cpp's updateTouchReadout(), and main.cpp's debugDump(). The
// GT911 clock-stretches and does not tolerate overlapping transactions, so
// concurrent readers held SCL low and wedged loop() permanently (screen and
// buttons both froze).
//
// In this design:
//   * touch::poll() is the ONLY function that touches the bus.
//   * touch::point() returns the cached result of the last poll, for anybody
//     else who wants the coordinates, and never performs I/O.
//   * the LVGL indev callback calls poll() (it runs from lv_timer_handler),
//     and the main loop calls pollIfDue() which is a no-op unless the poll
//     interval has elapsed. Both funnel through the same single path, and the
//     interval cap prevents the UI timer's variable rate from turning into a
//     bus flood.
#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace touch {

// Power on the bus, reset and identify the controller, register the LVGL indev.
// Returns false if no GT911 was found (the UI must still run: the device can be
// driven by the web API alone).
bool begin();

// The one and only bus reader. Returns true if a finger is currently down and
// fills *x/*y with panel coordinates (0..479 / 0..271).
bool poll();

// Rate-limited wrapper around poll(): performs a poll only if at least
// `interval_ms` have elapsed since the previous one. Safe to call every pass of
// loop(). Returns true if this call actually performed a poll.
bool pollIfDue(uint32_t interval_ms);

// Cached state from the most recent poll(). Never performs I/O.
bool point(uint16_t *x, uint16_t *y);

// Drop any reported press and clear the cached coordinates.
//
// MUST be called on every screen transition. LVGL's indev holds an internal
// pointer to the object it is currently "pressing"; if the screen changes while
// that pointer is set, the matching RELEASED is delivered to a widget that is no
// longer visible and the new screen receives no input at all. This is the fix
// for "touch works on the menu but nothing works after navigating".
void forceRelease();

// True if the controller was identified at boot.
bool available();

// Human-readable one-line health summary for the heartbeat log.
void logHealth();

// Log a full register dump (identity, 0x814E, config, i2c counters).
void dumpRegisters();

// Calibration aid: log every raw (unmapped) sample at ~5 Hz while enabled, plus
// the mapped result. Touch each corner and read the pair from the log.
void setRawTrace(bool on);
bool rawTrace();

// --- Runtime axis calibration ----------------------------------------------
//
// The compile-time TOUCH_FLIP_* / TOUCH_SWAP_XY defaults in board_config.h may
// not match a given unit: the digitizer and the glass are separate parts and can
// be mounted rotated relative to each other. Rather than guess and reflash,
// these let the operator cycle the transform on the device and SEE the result.
//
// `mode` cycles: 0 = none, 1 = flip X, 2 = flip Y, 3 = flip X+Y,
//                4 = swap XY, 5 = swap+flip X, 6 = swap+flip Y, 7 = swap+both.
void setAxisMode(uint8_t mode);
uint8_t axisMode();
const char *axisModeName(uint8_t mode);
uint8_t axisModeCount();

// Statistics for the diagnostics screen.
struct Stats {
    uint32_t polls;         // total poll() calls that touched the bus
    uint32_t samples;       // polls that returned a point
    uint32_t releases;      // genuine release frames from the controller
    uint32_t forced_releases; // releases synthesised on a screen change
    uint32_t bus_errors;    // consecutive rejected transactions
    uint8_t  last_status;   // last raw 0x814E
    uint16_t last_x;
    uint16_t last_y;
};
Stats stats();

}  // namespace touch
