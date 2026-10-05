// gt911_i2c.h — the GT911's private I2C bus.
//
// WHY THIS MODULE EXISTS AND OWNS Wire.begin()
//
// The previous design handed bus ownership to the vendor TAMC_GT911 library
// (which calls Wire.begin() inside its own begin()) and only pre-sized the Wire
// buffers. That made the bus's configuration implicit, unloggable, and
// impossible to verify before the driver ran. It also meant the documented
// "Wire.setTimeOut(50)" safety net was described in docs/ but never actually
// executed anywhere in the code.
//
// This module is now the single owner of the touch bus:
//   * starts it once, with explicit pins / 400 kHz / RX buffer / timeout
//   * can scan the bus before any driver logic runs (proves whether the part
//     answers at all, independent of register interpretation)
//   * exposes raw register read/write used by gt911.cpp
//
// The timeout is the important safety property: the GT911 clock-stretches, and
// without a timeout a stuck transaction blocks whatever task called it forever
// (that is exactly how the old build wedged loop()).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gt911_i2c {

// Start the bus (idempotent). Bus must be started before any read/write.
void begin();

// True once begin() has run successfully.
bool started();

// Probe one address with a zero-length write. Returns true if it ACKs.
bool ping(uint8_t addr);

// Scan 0x03..0x77 and log every address that ACKs. Returns the count.
int scan();

// Register read/write on the 16-bit register space. Return false on any bus
// error; never emit a partial read.
bool read(uint8_t addr, uint16_t reg, uint8_t *buf, size_t len);
bool write8(uint8_t addr, uint16_t reg, uint8_t val);

// Convenience single-byte read.
bool read8(uint8_t addr, uint16_t reg, uint8_t *val);

// Counters for the diagnostics screen / heartbeat.
struct Stats {
    uint32_t reads_ok;
    uint32_t reads_fail;
    uint32_t writes_ok;
    uint32_t writes_fail;
};
Stats stats();
void  resetStats();

}  // namespace gt911_i2c
