// display_driver.h — ILI6485 480x272 RGB panel + LVGL integration.
#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace display {

// Bring up the RGB panel and register the LVGL display with draw buffers.
// Call after lv_init().
bool begin();

// Backlight control (GPIO2, active high on this module).
//
// Implemented with LEDC PWM, because there is no dedicated brightness line: the
// pin switches the whole backlight, so dimming is done by duty cycle. See
// board_config.h for the frequency/resolution and why they matter.
void backlight(bool on);

// Set the backlight level, 0..100 percent, where 0 is fully off.
//
// Values are clamped. `backlight(true)` is equivalent to setBrightness(100);
// the startup level is LCD_BL_START_PERCENT.
void setBrightness(uint8_t percent);

// Current level, 0..100. Never performs I/O.
uint8_t brightness();

// Paint a colour-bar test pattern straight into the panel, bypassing LVGL.
// Use this to prove the RGB bus before trusting any "blank screen" report:
// a pattern that appears means pins/timings/polarity are all correct and the
// fault is upstream in LVGL or the flush path.
bool selfTest();

// Run LVGL timers/handler. Call from the main loop. This is also the only place
// LVGL's tree is redrawn from.
void tick();

// Statistics for the heartbeat log.
struct Stats {
    uint32_t flushes;         // flush callbacks served since boot
    uint32_t full_flushes;    // flushes that covered the whole panel
    uint32_t partial_flushes; // flushes that covered part of it
    uint32_t last_area_px;    // area of the most recent flush, in pixels
};
Stats stats();

// Log panel + flush statistics.
void logHealth();

}  // namespace display
