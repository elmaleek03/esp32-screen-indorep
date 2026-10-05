// gt911_i2c.cpp — owns the GT911 I2C bus and provides raw register access.
#include "gt911_i2c.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_config.h"

namespace gt911_i2c {

// The GT911 config block is 185 bytes (0x8047..0x80FF) and a 5-point read of
// the point area is 40 bytes. Wire's default RX buffer is 128, which silently
// clamps the config read. 256 covers everything we ever ask for in one go.
static const size_t kBufferSize = 256;

// Cap any single transaction. The GT911 clock-stretches; without this a stuck
// bus blocks the calling task forever. The old firmware described this in its
// docs but never actually called it — the wedge that froze the UI was a direct
// consequence.
static const uint32_t kTimeoutMs = 50;

static bool     s_started = false;
static Stats    s_stats   = {};

void begin() {
    if (s_started) return;

    // setBufferSize must precede Wire.begin(): this core allocates the buffers
    // during begin(), so sizing afterwards is either ignored or re-allocates.
    Wire.setBufferSize(kBufferSize);

    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, TOUCH_I2C_FREQ);
    Wire.setTimeOut(kTimeoutMs);

    s_started = true;
    Serial.printf("[i2c] bus up: sda=%d scl=%d freq=%u rxbuf=%u timeout=%ums\n",
                  PIN_TOUCH_SDA, PIN_TOUCH_SCL, (unsigned)TOUCH_I2C_FREQ,
                  (unsigned)kBufferSize, (unsigned)kTimeoutMs);
}

bool started() { return s_started; }

bool ping(uint8_t addr) {
    if (!s_started) return false;
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

int scan() {
    if (!s_started) {
        Serial.println("[i2c] scan requested before begin()");
        return 0;
    }

    int found = 0;
    for (uint8_t a = 0x03; a < 0x78; a++) {
        if (ping(a)) {
            Serial.printf("[i2c]   device ACK @0x%02X%s\n", a,
                          a == TOUCH_I2C_ADDR_PRIMARY   ? "  <- GT911 primary"
                          : a == TOUCH_I2C_ADDR_SECONDARY ? "  <- GT911 secondary"
                                                          : "");
            found++;
        }
        // Yield so the Wi-Fi task is not starved by a 117-address sweep.
        if ((a & 0x0F) == 0) delay(1);
    }
    Serial.printf("[i2c] scan done: %d device(s)\n", found);
    return found;
}

bool write8(uint8_t addr, uint16_t reg, uint8_t val) {
    if (!s_started) {
        s_stats.writes_fail++;
        return false;
    }
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    const uint8_t err = Wire.endTransmission();   // true STOP
    if (err != 0) {
        s_stats.writes_fail++;
        return false;
    }
    s_stats.writes_ok++;
    return true;
}

// STOP-then-read. A repeated start (endTransmission(false)) is NOT used: on
// this core a non-stop endTransmission() does not reliably transmit the
// register address, and a failed combined transfer can latch the bus into a
// nonStop state, breaking every later transaction. The vendor library and the
// vendor demo both use STOP-then-read, and that is the sequence proven to work
// on this module.
bool read(uint8_t addr, uint16_t reg, uint8_t *buf, size_t len) {
    if (!s_started || buf == nullptr || len == 0) {
        s_stats.reads_fail++;
        return false;
    }
    if (len > kBufferSize) {
        Serial.printf("[i2c] read of %u bytes exceeds rxbuf %u\n",
                      (unsigned)len, (unsigned)kBufferSize);
        s_stats.reads_fail++;
        return false;
    }

    Wire.beginTransmission(addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission() != 0) {
        s_stats.reads_fail++;
        return false;
    }

    const size_t got = Wire.requestFrom((int)addr, (int)len);
    if (got != len) {
        // Drain whatever arrived so a partial read cannot desync the next one.
        while (Wire.available()) Wire.read();
        s_stats.reads_fail++;
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (!Wire.available()) {
            s_stats.reads_fail++;
            return false;
        }
        buf[i] = (uint8_t)Wire.read();
    }
    s_stats.reads_ok++;
    return true;
}

bool read8(uint8_t addr, uint16_t reg, uint8_t *val) {
    return read(addr, reg, val, 1);
}

Stats stats() { return s_stats; }

void resetStats() { s_stats = Stats{}; }

}  // namespace gt911_i2c
