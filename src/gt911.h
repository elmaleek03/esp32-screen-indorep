// gt911.h — self-contained GT911 capacitive touch driver.
//
// WHY NOT THE VENDOR TAMC_GT911 LIBRARY
//
// The vendor library is unusable on this module for four independent reasons,
// each of which was verified by reading its source rather than guessed at:
//
//  1. read() ends with an unconditional `writeByteData(GT911_POINT_INFO, 0)`,
//     which clears the data-ready flag even when the controller has no new
//     sample. On a clock-stretching part polled from a UI timer that races the
//     controller's own frame rate and can drop every real sample.
//  2. Every helper ignores the I2C return value, so a NAK is indistinguishable
//     from a genuine zero. There is no way to tell "not touched" from "bus
//     broken" — which is exactly the ambiguity that made this bug hard to find.
//  3. reset() always calls setResolution(480, 272). That is correct for this
//     panel, but combined with readPoint()'s `x = width - x` it INVERTS both
//     axes a second time, so the library's ROTATION_NORMAL output is mirrored
//     relative to what the vendor demo's own map() then expects.
//  4. It latches its I2C address from the INT pin level during reset. INT is
//     not wired on this module, so it relies on the part's strapping instead.
//
// The driver below talks to the registers directly, checks every transaction,
// only consumes samples the controller flagged as ready, and applies exactly
// one axis mapping (in touch_driver.cpp).
#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace gt911 {

struct Point {
    uint16_t x;
    uint16_t y;
    uint16_t size;
    uint8_t  id;
};

// Reset the controller, probe both possible addresses, verify the product id,
// and program the output resolution. Returns true when the part is usable.
bool begin();

// True once begin() succeeded.
bool ready();

// The address the part answered on (0x5D or 0x14), or 0 if unknown.
uint8_t address();

// Product id string as reported at 0x8140 (4 bytes, NUL-terminated), e.g. "911".
const char *productId();

// Read the current state. Returns the number of points (0 when none, or when
// no new sample was available). `status` receives the raw 0x814E byte.
//
// `bus_ok` (optional) distinguishes the two very different reasons for a zero
// return: false means the I2C transaction itself failed (a dead or stuck bus),
// true means the transaction succeeded and the controller simply had no new
// sample. Conflating these is what made the original bug hard to see.
// This performs I2C traffic and must only be called by the single touch reader.
uint8_t readPoints(Point *out, uint8_t maxPoints, uint8_t *status, bool *bus_ok = nullptr);

// Log identity + a register dump. Safe to call at any time after begin().
void dumpIdentity();

// Round-trip a software reset via the command register and re-program config.
bool softReset();

}  // namespace gt911
