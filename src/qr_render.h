// qr_render.h — QRIS string -> LVGL QR canvas.
#pragma once

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>

namespace qr {

// Create the QR widget as a child of `parent`, positioned at (x,y) and sized
// `size` x `size`. Returns the widget (an lv_canvas-backed lv_qrcode).
lv_obj_t *create(lv_obj_t *parent, int x, int y, int size);

// Encode `data` into the QR widget and flag the object dirty so LVGL redraws.
// Returns false when the string is too long for the encoder at this size.
bool update(lv_obj_t *qrobj, const char *data);

// Size in pixels needed for `data` to be scannable, given a quiet zone of one
// module on each side. Returns 0 when the string cannot be encoded at all.
int requiredSize(const char *data);

// Hide/show, used when a payload carries no QR (cash flow: summary only).
void setVisible(lv_obj_t *qrobj, bool visible);

}  // namespace qr
