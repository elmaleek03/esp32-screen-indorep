// qr_render.cpp — QRIS string -> LVGL QR canvas.
//
// The QR is generated entirely on-device: LVGL's lv_qrcode (a canvas backed by
// LV_USE_QRCODE, which is enabled in lv_conf.h). No network round-trip and no
// image decoding, which matters because the backend has a 5 s HTTP timeout.
//
// IMPORTANT IMPLEMENTATION CONSTRAINTS (both are properties of lv_qrcode, and
// both bit this code during bring-up):
//
//  1. The canvas memory is allocated ONCE in lv_qrcode_constructor(), sized
//     from the `size` passed to lv_qrcode_create(). Calling lv_obj_set_size()
//     afterwards changes the widget's footprint but NOT the buffer, so the
//     drawn code keeps its original resolution. The canvas therefore has a
//     fixed size and lv_qrcode_update() centres the modules inside it. That is
//     also why create() must be called with the FINAL size.
//
//  2. lv_qrcode allocates its image buffer and the two QR scratch buffers from
//     LVGL's own heap (lv_mem_alloc), NOT from PSRAM, and it uses
//     qrcodegen_Ecc_MEDIUM. A 216x216 1-bit canvas is (216*216)/8 + 8 = 5840
//     bytes, plus ~1.4 KB of scratch during an update. LV_MEM_SIZE must cover
//     that; see lv_conf.h.
//
// The module count for a 1-bit indexed canvas is 4*version + 17; a full EMVCo
// QRIS string is typically 200-400 chars, which lands around version 10-15
// (57-77 modules). At 216 px that is ~2.8-3.8 px per module, which is
// comfortably scannable on this panel when the code is given a white quiet
// zone (lv_qrcode already centres with a margin, and the widget background is
// forced opaque white below).
#include "qr_render.h"

#include <Arduino.h>
#include <lvgl.h>
#include <stdlib.h>
#include <string.h>

#include "qrcodegen.h"   // vendored with LVGL; provides the qrcodegen_* encoder API

namespace qr {

namespace {
constexpr int kQuietZone = 1;   // modules of white border accounted for in sizing
}  // namespace

lv_obj_t *create(lv_obj_t *parent, int x, int y, int size) {
    if (parent == nullptr) return nullptr;

    // `size` here is FINAL: lv_qrcode allocates its canvas buffer from this
    // argument in its constructor and never resizes it. See the file header.
    lv_obj_t *obj = lv_qrcode_create(parent, (lv_coord_t)size,
                                     lv_color_black(), lv_color_white());
    if (obj == nullptr) {
        Serial.printf("[qr] lv_qrcode_create(%d) FAILED - raise LV_MEM_SIZE?\n", size);
        return nullptr;
    }
    lv_obj_set_pos(obj, x, y);
    // The QR must sit on an opaque white tile: transposing a dark theme's
    // background through the quiet zone is the classic "QR won't scan" mistake.
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    const size_t canvasBytes = (size_t)(size * size) / 8 + 8;
    Serial.printf("[qr] canvas created: %dx%d, %u bytes from the LVGL heap\n",
                  size, size, (unsigned)canvasBytes);
    return obj;
}

bool update(lv_obj_t *qrobj, const char *data) {
    if (qrobj == nullptr || data == nullptr || data[0] == '\0') {
        return false;
    }

    const lv_res_t res = lv_qrcode_update(qrobj, data, (uint32_t)strlen(data));
    if (res != LV_RES_OK) {
        Serial.printf("[qr] lv_qrcode_update FAILED for %u-char payload\n",
                      (unsigned)strlen(data));
        return false;
    }

    lv_obj_invalidate(qrobj);
    Serial.printf("[qr] encoded %u chars\n", (unsigned)strlen(data));
    return true;
}

int requiredSize(const char *data) {
    if (data == nullptr || data[0] == '\0') return 0;

    // Both buffers must be sized for the largest version we are willing to try.
    // 4 KB covers version 40 comfortably (buffer len max is 3918 bytes).
    static constexpr size_t kMaxBuf = qrcodegen_BUFFER_LEN_MAX;
    uint8_t *temp = (uint8_t *)malloc(kMaxBuf);
    uint8_t *qr   = (uint8_t *)malloc(kMaxBuf);
    if (temp == nullptr || qr == nullptr) {
        free(temp);
        free(qr);
        Serial.println("[qr] requiredSize: out of memory");
        return 0;
    }

    int result = 0;
    // Walk up versions until the payload fits, then size it for >= 4 px/module.
    for (int version = qrcodegen_VERSION_MIN; version <= qrcodegen_VERSION_MAX; version++) {
        if (!qrcodegen_encodeText(data, temp, qr, qrcodegen_Ecc_LOW,
                                  version, version, qrcodegen_Mask_AUTO, true)) {
            continue;
        }
        const int modules = 4 * version + 17 + 2 * kQuietZone;
        const int px = modules * 4;
        Serial.printf("[qr] payload fits version %d (%d modules) -> %d px\n",
                      version, modules, px);
        result = px;
        break;
    }

    free(temp);
    free(qr);

    if (result == 0) {
        Serial.printf("[qr] payload too large to encode (%u chars)\n",
                      (unsigned)strlen(data));
    }
    return result;
}

void setVisible(lv_obj_t *qrobj, bool visible) {
    if (qrobj == nullptr) return;
    if (visible) {
        lv_obj_clear_flag(qrobj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(qrobj, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace qr
