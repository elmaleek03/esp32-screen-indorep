// gt911.cpp — register-level GT911 driver. See gt911.h for why the vendor
// library is not used.
//
// REGISTER MAP (only what is used here)
//
//   0x8040  command          (write-only; 1 = soft reset, 2 = re-read config)
//   0x8047  config start     (186 bytes, 0x8047..0x8100 inclusive)
//   0x8048  X output max low
//   0x8049  X output max high
//   0x804A  Y output max low
//   0x804B  Y output max high
//   0x80FF  config checksum  (must make the 184-byte sum + cksum == 0 mod 256)
//   0x8100  config fresh flag (write 1 to make the controller latch the config)
//   0x8140  product id       (4 bytes ASCII, e.g. "911\0")
//   0x8144  firmware version
//   0x814E  point info       (bit7 = buffer/data ready, bits[3:0] = point count)
//   0x814F  point 1          (7 bytes: id, x_lo, x_hi, y_lo, y_hi, size_lo,
//                             size_hi) — then +8 per additional point
//
// Two off-by-one traps are deliberately encoded here, both of which cost time
// on this hardware:
//   * the config block is read from 0x8047 for 186 bytes so that the checksum
//     byte at 0x80FF and the fresh flag at 0x8100 are both inside the buffer
//     (0x8100 - 0x8047 = 0xB9 = 185, so 186 bytes covers it);
//   * point data starts at 0x814F, NOT 0x8150. Starting at 0x8150 shifts the
//     track id into the X value and makes every coordinate wrong.
#include "gt911.h"

#include <Arduino.h>

#include "board_config.h"
#include "gt911_i2c.h"

namespace gt911 {

namespace {

constexpr uint16_t kRegCommand   = 0x8040;
constexpr uint16_t kRegConfig    = 0x8047;
constexpr uint16_t kCfgOffXMaxLo = 0x8048 - kRegConfig;   // 1
constexpr uint16_t kCfgOffXMaxHi = 0x8049 - kRegConfig;   // 2
constexpr uint16_t kCfgOffYMaxLo = 0x804A - kRegConfig;   // 3
constexpr uint16_t kCfgOffYMaxHi = 0x804B - kRegConfig;   // 4
constexpr uint16_t kCfgOffTouchNum = 0x804C - kRegConfig; // 5
constexpr uint16_t kRegCksum     = 0x80FF;
constexpr uint16_t kRegFresh     = 0x8100;
constexpr uint16_t kRegProduct   = 0x8140;
constexpr uint16_t kRegFirmware  = 0x8144;
constexpr uint16_t kRegPointInfo = 0x814E;
constexpr uint16_t kRegPoint1    = 0x814F;
constexpr uint8_t  kPointStride  = 8;

// 0x8047..0x8100 inclusive.
constexpr size_t kConfigLen = 186;

uint8_t  s_addr = 0;
bool     s_ok   = false;
char     s_product[5] = {0};
uint8_t  s_config[kConfigLen] = {0};

// Pulse RST low, hold, release, and wait for the controller to boot. The INT
// pin selects the I2C address (low -> 0x5D, high -> 0x14); INT is not wired on
// this module, so both addresses are probed afterwards instead of relying on it.
void hardwareReset() {
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(12);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(60);
    pinMode(PIN_TOUCH_RST, INPUT);   // let the board's pull-up hold it released
    delay(60);
    Serial.printf("[gt911] hw reset done (rst=%d held 12ms, settled 120ms)\n",
                  PIN_TOUCH_RST);
}

// The product id at 0x8140 is the only trustworthy proof of life: an address
// ACK alone can come from a stuck-bus artefact or another device.
bool readProduct(uint8_t addr) {
    uint8_t buf[4] = {0};
    if (!gt911_i2c::read(addr, kRegProduct, buf, sizeof(buf))) {
        Serial.printf("[gt911] 0x%02X: product-id read FAILED\n", addr);
        return false;
    }
    snprintf(s_product, sizeof(s_product), "%c%c%c",
             buf[0] ? buf[0] : '.', buf[1] ? buf[1] : '.', buf[2] ? buf[2] : '.');
    Serial.printf("[gt911] 0x%02X: product id \"%s\" (raw %02X %02X %02X %02X)\n",
                  addr, s_product, buf[0], buf[1], buf[2], buf[3]);
    // "911" is the only id this panel reports. Anything else is not our part.
    return buf[0] == '9' && buf[1] == '1' && buf[2] == '1';
}

// Write X/Y output max into the cached config image and recompute the checksum
// exactly the way the controller does: sum the first 184 bytes, then store the
// two's complement in byte 184 (register 0x80FF).
void patchResolution(uint16_t w, uint16_t h) {
    s_config[kCfgOffXMaxLo] = (uint8_t)(w & 0xFF);
    s_config[kCfgOffXMaxHi] = (uint8_t)(w >> 8);
    s_config[kCfgOffYMaxLo] = (uint8_t)(h & 0xFF);
    s_config[kCfgOffYMaxHi] = (uint8_t)(h >> 8);
}

uint8_t computeChecksum() {
    uint8_t sum = 0;
    for (size_t i = 0; i < kConfigLen - 2; i++) sum += s_config[i];
    return (uint8_t)(~sum + 1);   // two's complement
}

bool readConfig() {
    if (!gt911_i2c::read(s_addr, kRegConfig, s_config, kConfigLen)) {
        Serial.println("[gt911] config read FAILED");
        return false;
    }
    Serial.printf("[gt911] config read ok: ver=0x%02X xmax=%u ymax=%u "
                  "touch=%u cksum=0x%02X fresh=%u\n",
                  s_config[0],
                  (unsigned)(s_config[kCfgOffXMaxLo] | (s_config[kCfgOffXMaxHi] << 8)),
                  (unsigned)(s_config[kCfgOffYMaxLo] | (s_config[kCfgOffYMaxHi] << 8)),
                  s_config[kCfgOffTouchNum],
                  s_config[kConfigLen - 2], s_config[kConfigLen - 1]);
    return true;
}

// Program the config back: checksum first, then the fresh flag. The controller
// only latches the block when the fresh flag transitions, so ordering matters.
bool commitConfig() {
    // The touch panel reports CRISP 5-point data, matching the vendor demo.
    s_config[kCfgOffTouchNum] = 5;

    const uint8_t cksum = computeChecksum();
    if (!gt911_i2c::write8(s_addr, kRegCksum, cksum)) {
        Serial.println("[gt911] checksum write FAILED");
        return false;
    }
    if (!gt911_i2c::write8(s_addr, kRegFresh, 1)) {
        Serial.println("[gt911] fresh-flag write FAILED");
        return false;
    }
    Serial.printf("[gt911] config committed: xmax=%u ymax=%u touch=%u cksum=0x%02X\n",
                  (unsigned)(s_config[kCfgOffXMaxLo] | (s_config[kCfgOffXMaxHi] << 8)),
                  (unsigned)(s_config[kCfgOffYMaxLo] | (s_config[kCfgOffYMaxHi] << 8)),
                  s_config[kCfgOffTouchNum], cksum);
    return true;
}

// The controller latches the fresh flag on a 0->1 transition, so if it is
// already 1 from a previous boot the write above is a no-op. Clear it first.
bool clearFreshFlag() {
    uint8_t fresh = 0;
    if (!gt911_i2c::read8(s_addr, kRegFresh, &fresh)) return false;
    if (fresh == 0) return true;
    Serial.printf("[gt911] fresh flag was already %u, clearing\n", fresh);
    return gt911_i2c::write8(s_addr, kRegFresh, 0);
}

bool sendCommand(uint8_t cmd) {
    if (!gt911_i2c::write8(s_addr, kRegCommand, cmd)) {
        Serial.printf("[gt911] command 0x%02X FAILED\n", cmd);
        return false;
    }
    return true;
}

}  // namespace

bool begin() {
    // The bus may already be started by another module; begin() is idempotent.
    gt911_i2c::begin();

    hardwareReset();

    // The GT911 answers at one of two addresses. Probe both and require a valid
    // product id; a bare ACK is not accepted as identification.
    const uint8_t candidates[] = {TOUCH_I2C_ADDR_PRIMARY, TOUCH_I2C_ADDR_SECONDARY};
    bool acked = false;

    for (uint8_t i = 0; i < 2 && !s_ok; i++) {
        const uint8_t addr = candidates[i];
        if (!gt911_i2c::ping(addr)) {
            Serial.printf("[gt911] 0x%02X: no ACK\n", addr);
            continue;
        }
        acked = true;
        Serial.printf("[gt911] 0x%02X: ACK, identifying...\n", addr);
        s_addr = addr;
        if (readProduct(addr)) {
            s_ok = true;
        } else {
            Serial.printf("[gt911] 0x%02X: not a GT911, ignoring\n", addr);
        }
    }

    if (!s_ok) {
        // Give the operator an actionable next step instead of a dead screen.
        Serial.println("[gt911] FATAL: no GT911 found.");
        if (!acked) {
            Serial.println("[gt911]   nothing ACKed at all -> check RST(GPIO38), "
                           "SDA(19)/SCL(20) and that USB-CDC is off.");
            Serial.println("[gt911]   full bus scan follows:");
            gt911_i2c::scan();
        } else {
            Serial.println("[gt911]   something ACKs but the id is wrong -> "
                           "lower TOUCH_I2C_FREQ to 100000 and retry.");
        }
        return false;
    }

    Serial.printf("[gt911] identified at 0x%02X, product \"%s\"\n", s_addr, s_product);

    // Firmware version is informational but a useful sanity signal.
    uint8_t fw = 0;
    if (gt911_i2c::read8(s_addr, kRegFirmware, &fw)) {
        Serial.printf("[gt911] firmware version byte 0x%02X\n", fw);
    }

    if (!clearFreshFlag() || !readConfig()) {
        Serial.println("[gt911] WARNING: config block unreadable; "
                       "continuing with controller defaults");
    } else {
        patchResolution(TOUCH_NATIVE_W, TOUCH_NATIVE_H);
        commitConfig();
    }

    // Consume any stale sample left over from the reset so the first real tap
    // is not mistaken for a stuck-ready condition.
    uint8_t st = 0;
    if (gt911_i2c::read8(s_addr, kRegPointInfo, &st) && (st & 0x80)) {
        gt911_i2c::write8(s_addr, kRegPointInfo, 0);
        Serial.printf("[gt911] cleared stale ready flag (status was 0x%02X)\n", st);
    }

    Serial.printf("[gt911] ready: %dx%d native reporting, status poll armed\n",
                  TOUCH_NATIVE_W, TOUCH_NATIVE_H);
    return true;
}

bool ready() { return s_ok; }

uint8_t address() { return s_addr; }

const char *productId() { return s_product; }

bool softReset() {
    if (!s_ok) return false;
    if (!sendCommand(1)) return false;   // 1 = soft reset
    delay(120);
    // The controller reloads its config from NVM on reset; re-program ours.
    if (!clearFreshFlag() || !readConfig()) return false;
    patchResolution(TOUCH_NATIVE_W, TOUCH_NATIVE_H);
    return commitConfig();
}

uint8_t readPoints(Point *out, uint8_t maxPoints, uint8_t *status, bool *bus_ok) {
    if (bus_ok) *bus_ok = false;

    if (!s_ok || out == nullptr || maxPoints == 0) {
        if (status) *status = 0;
        return 0;
    }

    uint8_t st = 0;
    if (!gt911_i2c::read8(s_addr, kRegPointInfo, &st)) {
        // The transaction itself failed: the bus is the problem, not the panel
        // being idle. Reported separately so the caller can count it.
        if (status) *status = 0;
        return 0;
    }
    if (status) *status = st;
    if (bus_ok) *bus_ok = true;

    const bool dataReady = (st & 0x80) != 0;
    uint8_t count = (uint8_t)(st & 0x0F);
    if (count > maxPoints) count = maxPoints;

    // Only consume the buffer when the controller said it has a complete set.
    // A cleared status is our acknowledgement and re-arms the controller.
    if (!dataReady) {
        return 0;
    }
    if (count == 0) {
        // Release frame: no finger down. Ack immediately.
        gt911_i2c::write8(s_addr, kRegPointInfo, 0);
        return 0;
    }

    for (uint8_t i = 0; i < count; i++) {
        uint8_t raw[7] = {0};
        const uint16_t reg = (uint16_t)(kRegPoint1 + i * kPointStride);
        if (!gt911_i2c::read(s_addr, reg, raw, sizeof(raw))) {
            // Partial read: ack and report nothing rather than emitting a
            // half-decoded coordinate.
            gt911_i2c::write8(s_addr, kRegPointInfo, 0);
            if (bus_ok) *bus_ok = false;
            return 0;
        }
        Point &p = out[i];
        p.id   = raw[0];
        p.x    = (uint16_t)(raw[1] | (raw[2] << 8));
        p.y    = (uint16_t)(raw[3] | (raw[4] << 8));
        p.size = (uint16_t)(raw[5] | (raw[6] << 8));
    }

    // Ack AFTER copying the points out, never before.
    gt911_i2c::write8(s_addr, kRegPointInfo, 0);
    return count;
}

void dumpIdentity() {
    if (!s_ok) {
        Serial.println("[gt911] dump: not initialised");
        return;
    }
    Serial.printf("[gt911] dump: addr=0x%02X product=\"%s\"\n", s_addr, s_product);

    uint8_t info[8] = {0};
    if (gt911_i2c::read(s_addr, kRegProduct, info, sizeof(info))) {
        Serial.printf("[gt911]   0x8140: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                      info[0], info[1], info[2], info[3],
                      info[4], info[5], info[6], info[7]);
    } else {
        Serial.println("[gt911]   0x8140 read FAILED");
    }

    // NOTE: this is a READ-ONLY inspection. The status byte at 0x814E is NOT
    // cleared here on purpose: clearing it is the host's acknowledgement of a
    // sample, and doing that out-of-band (e.g. while the operator's finger is
    // down) would silently swallow a real touch. readPoints() owns that write.
    uint8_t st = 0;
    if (gt911_i2c::read8(s_addr, kRegPointInfo, &st)) {
        Serial.printf("[gt911]   0x814E: 0x%02X (ready=%d cnt=%u)\n",
                      st, (st & 0x80) ? 1 : 0, st & 0x0F);
    } else {
        Serial.println("[gt911]   0x814E read FAILED");
    }

    uint8_t cfg[6] = {0};
    if (gt911_i2c::read(s_addr, kRegConfig, cfg, sizeof(cfg))) {
        Serial.printf("[gt911]   0x8047: ver=%02X xmax=%u ymax=%u touch=%u\n",
                      cfg[0],
                      (unsigned)(cfg[1] | (cfg[2] << 8)),
                      (unsigned)(cfg[3] | (cfg[4] << 8)), cfg[5]);
    } else {
        Serial.println("[gt911]   0x8047 read FAILED");
    }

    const gt911_i2c::Stats s = gt911_i2c::stats();
    Serial.printf("[gt911]   i2c: reads ok=%lu fail=%lu, writes ok=%lu fail=%lu\n",
                  (unsigned long)s.reads_ok, (unsigned long)s.reads_fail,
                  (unsigned long)s.writes_ok, (unsigned long)s.writes_fail);
}

}  // namespace gt911
