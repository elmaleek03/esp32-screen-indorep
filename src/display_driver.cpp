// display_driver.cpp — ILI6485 480x272 panel on the ESP32-S3 LCD_CAM
// (RPI-DPI / parallel RGB) peripheral, wired into LVGL v8.
//
// All pin and timing values come from the vendor demo:
//   4.3inch_ESP32-4827S043/1-Demo/Demo_Arduino/3_3-1_TFT_HelloWorld/HelloWorld/HelloWorld.ino
//
// The vendor demo drives the panel with Arduino_GFX's Arduino_RPi_DPI_RGBPanel.
// Here we use the underlying esp_lcd RGB panel driver directly (same peripheral,
// same timings) because LVGL needs a flush callback and PSRAM draw buffers.
//
// ---------------------------------------------------------------------------
// FLUSH ARCHITECTURE — the part that was wrong before
// ---------------------------------------------------------------------------
// The previous revision alternated between two broken designs, and its own
// comments record both symptoms (a "quarter of the screen", and a full freeze
// on the first touch). Both came from the same misunderstanding: with a single
// frame buffer, `esp_lcd_panel_draw_bitmap` does not return once the pixels are
// on the glass — it returns once the transfer has been *accepted*. So LVGL must
// not be told "flush complete" until that hand-off is really done, and it must
// not be allowed to start a second flush while the first is in flight.
//
// The design here, which is safe under ALL THREE refresh modes (see
// LCD_REFRESH_MODE in board_config.h):
//
//   * flushCb() arms `s_flush_pending` and clears `s_flush_done` BEFORE calling
//     draw_bitmap(), because the completion callback can fire from the ISR as
//     soon as the transfer starts. Arming afterwards would swallow the
//     completion and stall until the timeout.
//   * flushCb() does NOT call lv_disp_flush_ready(). It returns immediately.
//   * the completion callback (on_color_trans_done / on_frame_buf_complete) only
//     sets `s_flush_done`.
//   * tick(), in TASK context, waits (bounded) for `s_flush_done`, then calls
//     lv_disp_flush_ready() exactly once. LVGL therefore never renders into a
//     buffer the panel is still reading.
//   * the wait is capped, so a lost interrupt costs one torn frame rather than a
//     permanently frozen UI. That bound is what makes this safe even if the two
//     callbacks behave differently between the free-running and staged modes.
//
// FULL REFRESH is enabled on the LVGL driver (disp_drv.full_refresh = 1). That is
// the setting whose absence caused the intermittent tearing: without it LVGL
// renders in CHUNKS and hands flushCb() partial rectangles, and a partial write
// into a free-running frame buffer tears against the scanout — visible as a band
// of the old frame (typically the status bar) left at a different place on the
// screen. With it, every flushCb() is a whole frame and the two draw buffers
// behave as classic double buffering, which is the configuration LVGL's porting
// guide prescribes for an MCU that has an LCD controller peripheral. Do not try
// to emulate it by widening the area inside flushCb(); that was tried and is
// worse, because LVGL keeps rendering the next chunk into the buffer the widened
// transfer is still reading.
//
// REGRESSION NOTE: an intermediate revision replaced this with "release LVGL
// immediately in flushCb(), because num_fbs = 2 makes it safe". That was wrong:
// num_fbs rotates the PANEL's frame buffers, but there is only ONE LVGL DRAW
// buffer, and with bounce buffers draw_bitmap() returns before the DMA has
// finished reading it. The result was the panel showing a blend of two frames —
// seen in the field as the status bar tearing and reappearing at the bottom of
// the screen. Do not move the release back into flushCb().
//
// There is no mutex around the LVGL tree. LVGL v8 is not thread-safe and no lock
// makes it so, so instead this firmware guarantees every widget access happens in
// the main loop task: the web server never touches widgets, it publishes a
// payload that main.cpp consumes during its own pass.
#include "display_driver.h"

#include <Arduino.h>
#include <lvgl.h>

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_heap_caps.h"
// Provides LCD_CLK_SRC_DEFAULT (the enum behind lcd_clock_source_t, which
// hal/lcd_types.h only typedefs).
#include "soc/clk_tree_defs.h"

#include "board_config.h"

namespace display {

namespace {

esp_lcd_panel_handle_t s_panel = nullptr;

lv_disp_draw_buf_t s_draw_buf;
lv_color_t        *s_buf1 = nullptr;
// Second LVGL draw buffer. Required by full_refresh + two-buffer double
// buffering: LVGL renders into one while the DMA drains the other, so a
// full-frame transfer cannot be overwritten mid-flight.
lv_color_t        *s_buf2 = nullptr;

// The driver LVGL actually registered. Kept for the health log; flushCb()
// receives its own drv pointer, so the release no longer needs to round-trip
// through this.
lv_disp_drv_t     *s_registered_drv = nullptr;

// Set from the panel's completion ISR, and cleared in flushCb() BEFORE the
// transfer starts. flushCb() waits on this before releasing LVGL. It is the ONLY
// signal that the DMA has finished reading the draw buffer.
//
// Starting true means the first flush after boot is not made to wait for a
// callback that will never come.
volatile bool s_flush_done = true;

// Number of flushes seen since the last health log, for the diagnostic line.
uint32_t s_flush_outstanding = 0;

uint32_t s_flush_started_us = 0;
Stats    s_stats = {};

// Flushes that hit the bounded wait in flushCb() without a completion interrupt.
// Should stay at 0 in normal operation; a non-zero value means the panel's
// completion callback is being missed and each frame is costing the full timeout.
uint32_t s_flush_timeouts = 0;

// Why the glitch is diagnosable: record how MUCH is being flushed each time.
// A "full screen" flush is 480x272; anything else is a partial update, and a
// stream of partial updates against a free-running panel is the mechanism that
// produces visible tearing. Logging the area turns "it glitches" into a number.
uint32_t s_flush_area_px  = 0;   // area of the most recent flush, in pixels
uint32_t s_flush_full_cnt = 0;   // flushes that covered the whole panel
uint32_t s_flush_part_cnt = 0;   // flushes that covered part of it

#if LCD_BOUNCE_LINES > 0
// Bounce buffer sizing. See LCD_BOUNCE_LINES in board_config.h for why these
// exist (DMA bandwidth starvation once Wi-Fi competes for PSRAM).
//
// THE DRIVER REQUIRES AN EXACT DIVISOR: esp_lcd rejects the panel with "frame
// buffer size must be multiple of bounce buffer size" otherwise. The frame
// buffer is 480 * 272 = 130560 px, so the line count must divide 272
// (1, 2, 4, 8, 16, 17, 34, 68, 136, 272).
//
// The driver allocates TWO bounce buffers, so the cost is 2*lines*480*2 bytes:
//      8 lines ->  15.0 KB   (32 refills/frame - many interrupts)
//     17 lines ->  31.9 KB   (16 refills/frame)
//     34 lines ->  63.8 KB   ( 8 refills/frame)   <-- used
//     68 lines -> 127.5 KB   ( 4 refills/frame - too much internal DRAM left)
constexpr size_t kBounceLines = LCD_BOUNCE_LINES;
static_assert((LCD_HEIGHT % kBounceLines) == 0,
              "bounce buffer must divide the frame buffer exactly");
#endif

// Both of these are invoked from ISR context, so they do nothing but set a flag.
//
// Which one matters depends on whether bounce buffers are in play:
//   * with bounce buffers, `on_color_trans_done` fires when the user's draw
//     buffer has been copied into the bounce buffer — which is the moment the
//     draw buffer is safe to recycle, and therefore the correct time to release
//     LVGL. `on_frame_buf_complete` then fires later, when the internal frame
//     buffer is reusable.
//   * without bounce buffers, `on_color_trans_done` fires when the draw buffer
//     has been copied straight into the frame buffer.
//
// Both are registered and both set the same flag. Whichever arrives is a valid
// "the draw buffer is no longer being read" signal; flushCb arms the flag before
// starting a transfer, so the first completion wins and the second is a no-op.
bool onFlushComplete(esp_lcd_panel_handle_t panel,
                     const esp_lcd_rgb_panel_event_data_t *edata,
                     void *user_ctx) {
    (void)panel;
    (void)edata;
    (void)user_ctx;
    s_flush_done = true;
    return false;   // no high-priority task wake needed
}

void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    if (s_panel == nullptr) {
        // Nothing to draw into: release immediately or LVGL stalls forever.
        lv_disp_flush_ready(drv);
        return;
    }

    // ARM BEFORE STARTING. The completion callback can fire from the ISR as soon
    // as the transfer begins (especially for a small partial update that the
    // bounce-buffer DMA drains in microseconds). If we cleared the flag after
    // calling draw_bitmap(), that completion would be swallowed and we would sit
    // out the full timeout on every frame.
    s_flush_done = false;
    s_flush_started_us = micros();
    s_stats.flushes++;
    s_flush_outstanding++;

    // Record the flushed area, so the heartbeat's flush counters can be read as
    // "full frames" vs "partial updates" rather than as a meaningless total.
    const uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    const uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    s_flush_area_px = w * h;
    if (s_flush_area_px >= (uint32_t)LCD_WIDTH * LCD_HEIGHT) {
        s_flush_full_cnt++;
    } else {
        s_flush_part_cnt++;
    }

    // Inclusive 1-based end coordinates, as esp_lcd expects.
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px);

    // WAIT (bounded) FOR THE DMA TO FINISH READING `px`, THEN RELEASE LVGL —
    // exactly once, for THIS call.
    //
    // `px` points into the ONE draw buffer (s_buf1). draw_bitmap() with bounce
    // buffers only COPIES `px` into the bounce buffer and returns; the copy runs
    // on DMA afterwards, reading `px` the whole time. Releasing LVGL before that
    // finishes lets LVGL render the next frame into s_buf1 mid-copy, and the
    // panel shows a blend of two frames (seen as the status bar tearing and
    // reappearing at the bottom).
    //
    // The release MUST be here, keyed 1:1 to this flushCb call. An earlier
    // revision moved it into tick() and released once per tick instead — but LVGL
    // can call flushCb several times between two lv_timer_handler() runs, so that
    // released too few times and left LVGL's outstanding-flush count permanently
    // non-zero: the UI stopped updating (the digital clock froze) and the wedged
    // render loop slowed the whole firmware enough that HTTP requests timed out.
    //
    // The wait is BOUNDED: a lost completion interrupt costs one late frame, not
    // a frozen UI. This runs in the LVGL task, and the completion is signalled
    // from the panel ISR, so the loop is short.
    constexpr uint32_t kFlushTimeoutUs = 30000;   // 30 ms ~ 1 frame at 33 Hz
    while (!s_flush_done && (micros() - s_flush_started_us) < kFlushTimeoutUs) {
        // Yield briefly rather than spinning flat out. The completion is
        // signalled from the panel ISR, so this loop is normally only a few
        // dozen microseconds; yielding keeps the Wi-Fi/idle tasks alive even in
        // the pathological case where the ISR is missed and we sit here for the
        // full timeout. Without the yield, a missed ISR would monopolise the CPU
        // and HTTP requests would time out.
        delayMicroseconds(50);
    }
    if (!s_flush_done) {
        s_flush_timeouts++;
    }

    lv_disp_flush_ready(drv);
}

}  // namespace

bool begin() {
    Serial.println("[lcd] configuring RGB panel (ILI6485 480x272, 16-bit)");

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_PLL160M;

    cfg.timings.pclk_hz           = LCD_PCLK_HZ;
    cfg.timings.h_res             = LCD_WIDTH;
    cfg.timings.v_res             = LCD_HEIGHT;
    cfg.timings.hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH;
    cfg.timings.hsync_back_porch  = LCD_HSYNC_BACK_PORCH;
    cfg.timings.hsync_front_porch = LCD_HSYNC_FRONT_PORCH;
    cfg.timings.vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH;
    cfg.timings.vsync_back_porch  = LCD_VSYNC_BACK_PORCH;
    cfg.timings.vsync_front_porch = LCD_VSYNC_FRONT_PORCH;

    // These are the "idle low" flags. Arduino_GFX derives them from the
    // vendor's polarity arguments with inverted sense; see board_config.h.
    cfg.timings.flags.hsync_idle_low  = LCD_HSYNC_IDLE_LOW;
    cfg.timings.flags.vsync_idle_low  = LCD_VSYNC_IDLE_LOW;
    cfg.timings.flags.de_idle_high    = 0;
    cfg.timings.flags.pclk_active_neg = LCD_PCLK_ACTIVE_NEG;
    cfg.timings.flags.pclk_idle_high  = 0;

    // RGB565 parallel, matching Arduino_ESP32RGBPanel::getFrameBuffer().
    // That code sets data_width only and leaves bits_per_pixel at 0, which the
    // driver interprets as "same as data_width". Do the same.
    cfg.data_width     = 16;
    cfg.bits_per_pixel = 0;
    cfg.dma_burst_size = 64;

    // Refresh strategy, in one place. See the block comment in board_config.h
    // for why each value exists and which failure it maps to.
    cfg.num_fbs = LCD_CFG_NUM_FBS;

    // Bounce buffers, independent of the frame-buffer count. See the block
    // comment in board_config.h: without these the RGB DMA reads PSRAM directly
    // and starves as soon as Wi-Fi competes for bandwidth, which shows up as the
    // panel flashing/glitching while the web server is up.
    cfg.bounce_buffer_size_px = (LCD_BOUNCE_LINES > 0)
                                    ? (size_t)LCD_WIDTH * LCD_BOUNCE_LINES
                                    : 0;

    cfg.hsync_gpio_num = PIN_LCD_HSYNC;
    cfg.vsync_gpio_num = PIN_LCD_VSYNC;
    cfg.de_gpio_num    = PIN_LCD_DE;
    cfg.pclk_gpio_num  = PIN_LCD_PCLK;
    cfg.disp_gpio_num  = -1;   // no DISP line; backlight is a plain GPIO

    // data_gpio_nums[0] is the least significant bit of the pixel value.
    // RGB565: bits[4:0]=blue, bits[10:5]=green, bits[15:11]=red.
    cfg.data_gpio_nums[0]  = PIN_LCD_B0;
    cfg.data_gpio_nums[1]  = PIN_LCD_B1;
    cfg.data_gpio_nums[2]  = PIN_LCD_B2;
    cfg.data_gpio_nums[3]  = PIN_LCD_B3;
    cfg.data_gpio_nums[4]  = PIN_LCD_B4;
    cfg.data_gpio_nums[5]  = PIN_LCD_G0;
    cfg.data_gpio_nums[6]  = PIN_LCD_G1;
    cfg.data_gpio_nums[7]  = PIN_LCD_G2;
    cfg.data_gpio_nums[8]  = PIN_LCD_G3;
    cfg.data_gpio_nums[9]  = PIN_LCD_G4;
    cfg.data_gpio_nums[10] = PIN_LCD_G5;
    cfg.data_gpio_nums[11] = PIN_LCD_R0;
    cfg.data_gpio_nums[12] = PIN_LCD_R1;
    cfg.data_gpio_nums[13] = PIN_LCD_R2;
    cfg.data_gpio_nums[14] = PIN_LCD_R3;
    cfg.data_gpio_nums[15] = PIN_LCD_R4;

    cfg.flags.fb_in_psram        = 1;   // frame buffer in PSRAM
    cfg.flags.disp_active_low    = 0;
    // double_fb is set via num_fbs; leave the redundant flag clear so the two
    // cannot disagree.
    cfg.flags.double_fb          = 0;
    // In free-running modes the controller scans the frame buffer continuously,
    // so a write into it becomes visible at the next vsync with no extra step.
    cfg.flags.refresh_on_demand  = LCD_CFG_REFRESH_ON_DEMAND;

    const esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (err != ESP_OK) {
        Serial.printf("[lcd] esp_lcd_new_rgb_panel FAILED: %s\n",
                      esp_err_to_name(err));
        s_panel = nullptr;
        return false;
    }

    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = onFlushComplete;
    cbs.on_frame_buf_complete = onFlushComplete;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, nullptr));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    // --- LVGL draw buffers in PSRAM ---------------------------------------
    // TWO full-screen buffers (261 KB each, 522 KB total from an 8 MB pool).
    // full_refresh + two screen-sized buffers is the configuration LVGL's porting
    // guide prescribes for an MCU with an LCD controller peripheral (see the
    // disp_drv.full_refresh comment below). LVGL alternates them per frame, so
    // the DMA can drain one while LVGL renders into the other.
    const size_t px = (size_t)LCD_WIDTH * LCD_HEIGHT;
    s_buf1 = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t),
                                           MALLOC_CAP_SPIRAM);
    if (s_buf1 == nullptr) {
        Serial.println("[lcd] PSRAM draw buffer 1 alloc FAILED");
        return false;
    }
    s_buf2 = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t),
                                           MALLOC_CAP_SPIRAM);
    if (s_buf2 == nullptr) {
        // Fatal: full_refresh on a single buffer is the combination that tears.
        // Refuse to start rather than run in a known-bad configuration.
        Serial.println("[lcd] PSRAM draw buffer 2 alloc FAILED - full_refresh "
                       "needs two buffers");
        return false;
    }

    // --- Prime the panel ---------------------------------------------------
    // MANDATORY, and the reason the first attempt came up solid white: until a
    // draw_bitmap() has been issued, the LCD_CAM has no frame to send, so the
    // panel free-runs against whatever the frame buffer held at power-on
    // (uninitialised PSRAM, which reads as 0xFFFF = white).
    //
    // The vendor bus does exactly this: it pushes a single 1x1 pixel right after
    // init to start the refresh engine from a known state. Reproduce it, but push
    // a WHOLE black frame so the first thing on screen is deterministic rather
    // than one black pixel in a white field.
    //
    // With num_fbs > 1 the driver rotates between frame buffers, so EVERY buffer
    // must be primed: the first redraw switches scanout to the other one, and if
    // that one is still uninitialised the panel flashes white for a frame. Push
    // the black frame once per frame buffer.
    memset(s_buf1, 0x00, px * sizeof(lv_color_t));
    memset(s_buf2, 0x00, px * sizeof(lv_color_t));
    Serial.printf("[lcd] priming %u frame buffer(s) with black (%u px each)\n",
                  (unsigned)LCD_CFG_NUM_FBS, (unsigned)px);
    for (size_t i = 0; i < LCD_CFG_NUM_FBS; i++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_buf1);
    }

    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, px);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = LCD_WIDTH;
    disp_drv.ver_res  = LCD_HEIGHT;
    disp_drv.flush_cb = flushCb;
    disp_drv.draw_buf = &s_draw_buf;

    // --- full_refresh: the setting whose absence caused the tearing -----------
    // LVGL's porting guide (4.3inch_ESP32-4827S043/1-Demo/Demo_Arduino/
    // Libraries/Lvgl/docs/porting/display.md) is explicit:
    //
    //   "LVGL might render the screen in multiple chunks and therefore call
    //    flush_cb multiple times."
    //
    // Without full_refresh, flushCb() receives dirty CHUNKS, never whole frames.
    // A partial write into a FREE-RUNNING PSRAM frame buffer lands at an arbitrary
    // scan position, so the panel shows part of the old frame and part of the new
    // one: the top band tearing and reappearing at the bottom. That was the
    // intermittent glitch reported on this board and on its sibling project.
    //
    // The same guide names the correct configuration for this hardware:
    //
    //   "If full_refresh is enabled and two screen sized draw buffers are
    //    provided, LVGL's display handling works like 'traditional' double
    //    buffering. This means the flush_cb callback only has to update the
    //    address of the framebuffer. ... This configuration should be used if the
    //    MCU has an LCD controller peripheral."
    //
    // The ESP32-S3 has one (LCD_CAM), so this is the intended mode, not a
    // workaround. Verified on hardware in the sibling cooktaker project, where
    // the health log went from "full 0 part 63" (every flush partial) to whole
    // frames and the glitch disappeared.
    //
    // DO NOT emulate this by widening the area inside flushCb(). That was tried
    // and made it worse: LVGL kept rendering the next CHUNK into the buffer the
    // widened transfer was still reading.
    disp_drv.full_refresh = 1;

    // lv_disp_drv_register() returns the lv_disp_t and stores its OWN copy of
    // the driver struct. That copy is what the flush callback receives, so this
    // module must recover it via lv_disp_t::driver (the public member LVGL's own
    // refresh code uses) rather than keeping a pointer to our local template.
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    if (disp == nullptr || disp->driver == nullptr) {
        Serial.println("[lcd] lv_disp_drv_register FAILED");
        return false;
    }
    s_registered_drv = disp->driver;

    Serial.printf("[lcd] panel ready: %dx%d @ %u Hz, refresh=%s, fbs=%u, "
                  "bounce=%u lines (%u KB DRAM), full_refresh=1, "
                  "PSRAM draw buffers 2 x %u KB\n",
                  LCD_WIDTH, LCD_HEIGHT, (unsigned)LCD_PCLK_HZ,
                  LCD_MODE_NAME, (unsigned)LCD_CFG_NUM_FBS,
                  (unsigned)LCD_BOUNCE_LINES,
                  (unsigned)(2UL * LCD_BOUNCE_LINES * LCD_WIDTH * 2 / 1024),
                  (unsigned)(px * sizeof(lv_color_t) / 1024));
    return true;
}

// --- Backlight -------------------------------------------------------------
//
// The default is the vendor-proven path: a plain digitalWrite on GPIO2. PWM
// dimming is available behind LCD_BL_USE_PWM but is NOT the default, because on
// this board it produced a completely dark panel. See board_config.h for the
// reasoning and the likely circuit cause.
//
// The important property of this code: it can never leave the panel dark
// without saying so. Whichever path is taken, the log states what was chosen,
// and with PWM disabled the level is honest (0 = off, anything else = full on).
static uint8_t s_bl_percent = LCD_BL_START_PERCENT;

#if LCD_BL_USE_PWM
static bool     s_bl_pwm_ready = false;
static const uint32_t s_bl_max_duty = (1u << LCD_BL_LEDC_RES_BITS) - 1u;

static bool brightnessPwmInit() {
    if (s_bl_pwm_ready) return true;

    // ledcAttach(pin, freq, resolution) is the Arduino core 3.x API.
    if (!ledcAttach(PIN_LCD_BL, LCD_BL_LEDC_FREQ_HZ, LCD_BL_LEDC_RES_BITS)) {
        Serial.println("[lcd] backlight PWM attach FAILED; falling back to on/off");
        return false;
    }
    s_bl_pwm_ready = true;
    Serial.printf("[lcd] backlight PWM ready: pin=%d %u Hz, %u-bit\n",
                  PIN_LCD_BL, (unsigned)LCD_BL_LEDC_FREQ_HZ,
                  (unsigned)LCD_BL_LEDC_RES_BITS);
    return true;
}
#endif

// The always-available path: drive the pin as a plain on/off line.
static void backlightDigital(bool on) {
    // NOTE: ledcAttach() (if it ever ran) claims the pin for the LEDC
    // peripheral. A digitalWrite on a pin still owned by LEDC has no effect, so
    // the pin must be released first. This is done unconditionally because it is
    // cheap and it is the difference between "0% means dark" and "0% means stuck
    // at whatever duty LEDC last had".
    ledcDetach(PIN_LCD_BL);
    pinMode(PIN_LCD_BL, OUTPUT);
#if LCD_BL_ACTIVE_HIGH
    digitalWrite(PIN_LCD_BL, on ? HIGH : LOW);
#else
    digitalWrite(PIN_LCD_BL, on ? LOW : HIGH);
#endif
}

void setBrightness(uint8_t percent) {
    if (percent > 100) percent = 100;
    s_bl_percent = percent;

#if LCD_BL_USE_PWM
    if (brightnessPwmInit()) {
        // 0% must be a true zero, not a tiny duty that still glows.
        uint32_t duty = (uint32_t)(((uint64_t)s_bl_max_duty * percent) / 100u);
        if (percent == 0) duty = 0;
#if !LCD_BL_ACTIVE_HIGH
        duty = s_bl_max_duty - duty;   // active-low: 100% is a solid LOW
#endif
        ledcWrite(PIN_LCD_BL, duty);
        return;
    }
    // Attach failed: fall through to the digital path.
#endif

    // Without PWM there is no middle state, so be honest about it: 0 is off,
    // anything above 0 is full on. Pretending a 50% request produced 50% would
    // be a lie the operator cannot see through.
    backlightDigital(percent > 0);
}

uint8_t brightness() { return s_bl_percent; }

void backlight(bool on) {
    // Turning the panel off then back on must not lose the configured level.
    if (on) {
        setBrightness(s_bl_percent == 0 ? LCD_BL_START_PERCENT : s_bl_percent);
    } else {
        setBrightness(0);
    }
}

bool selfTest() {
    if (s_panel == nullptr || s_buf1 == nullptr) {
        Serial.println("[lcd] selftest: panel or draw buffer missing");
        return false;
    }

    // Paint the classic R G B Y C M W K bar strip plus a white border directly
    // into the draw buffer and push it, with NO LVGL involved.
    //
    // This splits the "screen is blank" diagnosis in two, which is the whole
    // point: if this pattern appears, the panel bus, timings and pins are all
    // correct and any later blank screen is an LVGL/flush problem. If it does
    // not appear, the fault is in the panel bring-up and no amount of LVGL work
    // will help.
    const uint16_t bars[8] = {
        0xF800,  // red
        0x07E0,  // green
        0x001F,  // blue
        0xFFE0,  // yellow
        0x07FF,  // cyan
        0xF81F,  // magenta
        0xFFFF,  // white
        0x0000,  // black
    };

    const size_t px = (size_t)LCD_WIDTH * LCD_HEIGHT;
    uint16_t *fb = (uint16_t *)s_buf1;
    const size_t barW = LCD_WIDTH / 8;
    const size_t barH = LCD_HEIGHT / 3;

    for (size_t y = 0; y < LCD_HEIGHT; y++) {
        for (size_t x = 0; x < LCD_WIDTH; x++) {
            uint16_t c;
            if (y < barH) {
                // Top third: the eight colour bars.
                size_t b = x / barW;
                if (b > 7) b = 7;
                c = bars[b];
            } else if (y < (barH * 2)) {
                // Middle third: grey ramp, proves all 6 bits per channel.
                const uint16_t v = (uint16_t)((x * 63) / (LCD_WIDTH - 1));
                c = (uint16_t)((v << 11) | (v << 5) | v);
            } else {
                // Bottom third: white border with black fill, proves geometry
                // (a shifted image shows up immediately as an uneven border).
                const bool border = (x < 4) || (x >= LCD_WIDTH - 4) ||
                                    (y < (barH * 2)) || (y >= LCD_HEIGHT - 4);
                c = border ? 0xFFFF : 0x0000;
            }
            fb[y * LCD_WIDTH + x] = c;
        }
    }

    Serial.println("[lcd] selftest: pushing colour-bar pattern (no LVGL)");
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_buf1);

    // Give the panel time to actually scan the frame out before LVGL takes over.
    delay(400);
    return true;
}

void tick() {
    // LVGL is released inside flushCb(), once per flush, after the bounded wait
    // for the panel's DMA to finish with the draw buffer (see flushCb). There is
    // therefore nothing to drain here: this function's only job is to run LVGL.
    //
    // Do NOT move the release back here "once per tick". LVGL can flush more than
    // one region between two calls to lv_timer_handler(), so releasing per tick
    // releases too few times and permanently wedges LVGL's outstanding-flush
    // counter — the UI then freezes (the digital clock was the first visible
    // casualty) and the slowed loop makes HTTP requests time out.
    lv_timer_handler();
}

Stats stats() {
    Stats s = s_stats;
    s.full_flushes    = s_flush_full_cnt;
    s.partial_flushes = s_flush_part_cnt;
    s.last_area_px    = s_flush_area_px;
    return s;
}

void logHealth() {
    // Report INTERNAL DRAM separately from total heap: the bounce buffers (in
    // the staged mode) and the Wi-Fi stack both live there, and a healthy total
    // heap can still hide internal-DRAM exhaustion.
    //
    // timeouts is the key field for the tearing fix: it is the number of flushes
    // tick() released WITHOUT seeing the completion interrupt. In a healthy run
    // it stays 0; if it climbs, the panel ISR is being missed and the flush is
    // being released late (one frame behind), not early.
    Serial.printf("[lcd] health: flushes=%lu (full=%lu partial=%lu) "
                  "outstanding=%lu timeouts=%lu last_area=%lu px scanning=%d "
                  "internal_free=%u KB (largest %u KB) psram_free=%u KB\n",
                  (unsigned long)s_stats.flushes,
                  (unsigned long)s_flush_full_cnt,
                  (unsigned long)s_flush_part_cnt,
                  (unsigned long)s_flush_outstanding,
                  (unsigned long)s_flush_timeouts,
                  (unsigned long)s_flush_area_px,
                  s_flush_done ? 0 : 1,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(ESP.getFreePsram() / 1024));
    s_flush_outstanding = 0;
}

}  // namespace display
