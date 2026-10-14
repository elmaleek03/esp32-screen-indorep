// board_config.h — hardware definition for the ESP32-4827S043 4.3" module.
//
// SOURCE OF TRUTH
//   Official vendor folder in this repo:
//     4.3inch_ESP32-4827S043/1-Demo/Demo_Arduino/3_3-1_TFT_HelloWorld/HelloWorld/HelloWorld.ino
//     4.3inch_ESP32-4827S043/1-Demo/Demo_Arduino/3_3-4_TFT-LVGL-Widgets/LvglWidgets/touch.h
//
//   MCU      : ESP32-S3-WROOM-1-N16R8 (16MB flash, 8MB PSRAM)
//   Panel    : 4.3" TFT, 480x272, driver IC ILI6485, 16-bit parallel RGB
//              driven by the ESP32-S3 LCD_CAM (RPI-DPI) peripheral
//   Touch    : GT911 capacitive, own I2C bus
//   Backlight: plain GPIO, no I/O expander on this board
//
// This module has NO CH422G I/O expander. Backlight and touch reset are direct
// GPIOs, and the touch I2C pins are 19/20 (not the 8/9 used by Waveshare
// ESP32-S3-Touch-LCD carriers), which is also why native USB cannot be used.
#pragma once

// ---------------------------------------------------------------------------
// Panel geometry and RGB timing
// ---------------------------------------------------------------------------
#define LCD_WIDTH          480
#define LCD_HEIGHT         272

// From Arduino_RPi_DPI_RGBPanel(...) in the official HelloWorld demo:
//   480, 0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 43 /* hsync_back_porch */
//   272, 0 /* vsync_polarity */, 8 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 12 /* vsync_back_porch */
//   1 /* pclk_active_neg */, 9000000 /* prefer_speed */, true /* auto_flush */
#define LCD_PCLK_HZ            9000000   // 9 MHz
#define LCD_HSYNC_FRONT_PORCH  8
#define LCD_HSYNC_PULSE_WIDTH  4
#define LCD_HSYNC_BACK_PORCH   43
#define LCD_VSYNC_FRONT_PORCH  8
#define LCD_VSYNC_PULSE_WIDTH  4
#define LCD_VSYNC_BACK_PORCH   12
#define LCD_PCLK_ACTIVE_NEG    1

// NOTE on polarity, this is counter-intuitive and got it wrong once:
// Arduino_GFX maps its `hsync_polarity` argument to esp_lcd's
// `hsync_idle_low` flag INVERTEDLY:
//     hsync_idle_low = (hsync_polarity == 0) ? 1 : 0;
// So the vendor's `0` means the HSYNC signal sits LOW while idle, i.e.
// idle_low = 1. Setting this backwards shifts/squashes the image (it produced
// a quarter-width picture). Keep these as "idle low" booleans, matching
// Arduino_ESP32RGBPanel::getFrameBuffer().
#define LCD_HSYNC_IDLE_LOW     1
#define LCD_VSYNC_IDLE_LOW     1

// ---------------------------------------------------------------------------
// RGB bus pins, from Arduino_ESP32RGBPanel(...) in the official demo.
// ---------------------------------------------------------------------------
#define PIN_LCD_DE      40
#define PIN_LCD_VSYNC   41
#define PIN_LCD_HSYNC   39
#define PIN_LCD_PCLK    42

#define PIN_LCD_R0      45
#define PIN_LCD_R1      48
#define PIN_LCD_R2      47
#define PIN_LCD_R3      21
#define PIN_LCD_R4      14

#define PIN_LCD_G0      5
#define PIN_LCD_G1      6
#define PIN_LCD_G2      7
#define PIN_LCD_G3      15
#define PIN_LCD_G4      16
#define PIN_LCD_G5      4

#define PIN_LCD_B0      8
#define PIN_LCD_B1      3
#define PIN_LCD_B2      46
#define PIN_LCD_B3      9
#define PIN_LCD_B4      1

// RGB565 bit order for esp_lcd data lines:
//   bits [4:0]   -> blue
//   bits [10:5]  -> green
//   bits [15:11] -> red
// PIN_LCD_Bx/Bx map to data_gpio_nums[0..4], Gx to [5..10], Rx to [11..15].

// ---------------------------------------------------------------------------
// Backlight: plain GPIO2, driven HIGH to illuminate (from the official demo's
// `#define TFT_BL 2`).
//
// There is no dedicated brightness-control hardware: GPIO2 drives the backlight
// enable through a transistor/resistor, so the only way to dim it is to switch
// that line with PWM.
//
// LCD_BL_USE_PWM selects how the backlight is driven:
//
//   0 (DEFAULT) = PLAIN ON/OFF.
//       digitalWrite on GPIO2, exactly as the vendor demo does. This is the
//       configuration PROVEN to light this panel. Use it unless you have
//       confirmed that PWM works on your specific board.
//
//   1 = PWM DIMMING via LEDC.
//       Dims by duty cycle. On THIS board this was tried and the panel went dark
//       (see below), so it is opt-in rather than default.
//
// WHY PWM IS NOT THE DEFAULT HERE
//
//   Setting LCD_BL_USE_PWM=1 on this unit resulted in a completely dark panel —
//   the backlight never lit. The likely cause is the backlight drive circuit:
//   many of these modules feed the backlight enable through a resistor and a
//   small transistor, and some of those circuits need a solid DC level to turn
//   on rather than a switched signal. Others use a dedicated backlight IC whose
//   enable pin latches, so a PWM train never registers as "on".
//
//   The failure is silent and total (dark screen), and the only way to recover
//   is to press 'b' on the serial console or reflash — which is a bad trade for
//   a cosmetic feature. So the proven on/off path stays the default.
//
//   If you want to retry PWM on a different batch of panels: set this to 1, and
//   if the screen stays dark, press 'b' a few times on the serial console (it
//   cycles 100% first) before concluding anything — the firmware logs whether
//   ledcAttach() succeeded.
//
// PWM frequency matters if enabled: below roughly 200 Hz a backlight visibly
// flickers. 5 kHz is far above the flicker-fusion threshold and also above the
// audible band.
// ---------------------------------------------------------------------------
#define PIN_LCD_BL      2
#define LCD_BL_ACTIVE_HIGH  1

#define LCD_BL_USE_PWM      0

// LEDC settings, used only when LCD_BL_USE_PWM is 1.
#define LCD_BL_LEDC_FREQ_HZ  5000
// 10-bit resolution: duty 0..1023, i.e. ~0.1% granularity.
#define LCD_BL_LEDC_RES_BITS 10

// Startup backlight level, in percent (0..100).
//
// With LCD_BL_USE_PWM 0 this is interpreted honestly: 0 = off, anything else =
// full on (there is no middle state without PWM). The console 'b' key cycles
// levels, and with PWM disabled it simply toggles on/off.
#define LCD_BL_START_PERCENT 100

// ---------------------------------------------------------------------------
// GT911 touch: own I2C bus, from touch.h in the official LVGL Widgets demo.
// TOUCH_GT911_INT is -1 (not wired), RST is GPIO38.
// NOTE: 19/20 are the ESP32-S3 native USB D-/D+, which is why this firmware
// must not enable USB-CDC; see platformio.ini.
// ---------------------------------------------------------------------------
#define PIN_TOUCH_SDA   19
#define PIN_TOUCH_SCL   20
#define PIN_TOUCH_INT   (-1)   // not connected on this module
#define PIN_TOUCH_RST   38

// I2C clock. The GT911 tolerates 400 kHz, but the vendor demo runs 100 kHz and
// the part clock-stretches during config writes. If the identity probe is flaky,
// drop this to 100000 — it is the single most effective bus-reliability knob.
#define TOUCH_I2C_FREQ  400000

// GT911 has two possible I2C addresses, selected by the INT pin level during
// reset release. INT is not wired here, so the part powers up on the address
// strapped by its own pull resistors. The vendor demo uses 0x5D (GT911_ADDR1);
// the driver probes BOTH and uses whichever answers.
#define TOUCH_I2C_ADDR_PRIMARY    0x5D
#define TOUCH_I2C_ADDR_SECONDARY  0x14

// ---------------------------------------------------------------------------
// Touch axis transform
// ---------------------------------------------------------------------------
// These describe what the DIGITIZER does relative to the GLASS, in plain terms.
// They are applied to the raw controller value:
//
//   TOUCH_FLIP_X  1 = mirror horizontally (raw 0 is the RIGHT edge of the glass)
//   TOUCH_FLIP_Y  1 = mirror vertically   (raw 0 is the BOTTOM edge of the glass)
//   TOUCH_SWAP_XY 1 = the axes are transposed (panel mounted rotated 90 degrees)
//
// Observed on this unit: touching the panel's bottom-right reported the
// top-left button, i.e. BOTH axes were mirrored by the end of the chain, so the
// correct setting here is NO transform — the digitizer's raw coordinates already
// increase toward the panel's bottom-right.
//
// Verified on hardware:
//   FLIP_X 1, FLIP_Y 1  -> bottom-right button hit from the top-left (180 off)
//   FLIP_X 0, FLIP_Y 0  -> correct
//
// There is no on-screen calibration page any more (deliberately: it was only
// needed once, and its buttons cluttered the menu). To re-check a different
// unit, use the serial console: press 'c' to trace raw -> mapped coordinates,
// and 'x' to cycle the transform live until the reported point matches the
// finger. Then write the winning combination here.
#define TOUCH_FLIP_X    0
#define TOUCH_FLIP_Y    0
#define TOUCH_SWAP_XY   0

// --- Touch raw range --------------------------------------------------------
// The visible panel and the touch digitizer do NOT have to cover the same area.
// This is the raw range the digitizer reports between the panel's two edges, in
// the order AFTER the flips above. If a tap at the panel's edge does not land at
// that edge, this is what to adjust. Set it from the 'c' console trace.
#define TOUCH_NATIVE_W  480
#define TOUCH_NATIVE_H  272

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------
// The device takes its address from DHCP. The router assigns it and the device
// shows the result on the "Terhubung!" (connected) screen, so the operator can
// read the live address off the panel without a serial console.
//
// WHY DHCP ONLY
//
//   An earlier revision tried to force a fixed address (DHCP-then-static
//   fallback to 192.168.4.252) to satisfy a backend that hardcodes
//   192.168.4.254. That static path did not work reliably here: the fallback
//   address is often already in use (or outside the router's lease pool), so the
//   claim conflicted and the device ended up unreachable — the exact failure it
//   was meant to prevent. DHCP is what actually works on this network, so the
//   static machinery is retired.
//
// CONSEQUENCE FOR THE BACKEND
//
//   The Go backend (indorep_backend_v2/display/display.go) hardcodes
//   http://192.168.4.254/. Because this device now sits on a DHCP address, the
//   backend can only reach it if one of these is true:
//     * the router's DHCP reservation gives this device's MAC the address the
//       backend expects (recommended — read the MAC off the connected screen or
//       the boot log and pin it), or
//     * the backend is repointed at the DHCP address shown on the device.
//   The device cannot solve this from its side; it just reports its address.
#define NET_IP_MODE          0

// Retained so net.cpp still compiles its (now unused at mode 0) static helpers
// and so the health endpoint can name the address the backend is configured to
// call. NOT applied to the interface any more.
#define NET_FALLBACK_ADDR    "192.168.4.252"
#define NET_FALLBACK_GW      "192.168.4.1"
#define NET_FALLBACK_MASK    "255.255.255.0"

// The address the Go backend will use. Kept as a separate symbol so the UI and
// the logs can state what the backend expects without implying the device
// necessarily has it.
#define NET_BACKEND_ADDR     "192.168.4.254"

#define WEB_SERVER_PORT      80

// ---------------------------------------------------------------------------
// Time (NTP) — used only by the analog clock on the standby screen
// ---------------------------------------------------------------------------
// The clock is the one thing in the UI that needs wall-clock time, so the device
// syncs over NTP once it has associated. Until the first sync succeeds the
// homescreen shows the clock face with a visible "unsynced" cue rather than a
// plausible-looking wrong time.
//
// DNS server handed to the stack alongside the static address.
//
// THIS IS REQUIRED FOR NTP. An earlier revision set no DNS server with the
// comment "the device is a server, not a client" — true until the standby clock
// was added. The SNTP client resolves NTP_SERVER_* by hostname, so with no DNS
// server configured the lookup fails and the clock never syncs, silently.
//
// The gateway is the right default: routers on these networks run a DNS
// resolver. Override with a specific server if your network needs one.
#define NET_DNS_SERVER    NET_FALLBACK_GW

// TZ offset is in SECONDS, and there is no DST term because Indonesia has none.
// WIB (Jakarta) is UTC+7 -> 7 * 3600 = 25200.
#define NTP_TZ_OFFSET_S   25200
#define NTP_TZ_DST_S      0

// NTP servers. configTime() accepts at most three. The first two are hostnames
// (the normal path); the third is a raw IP of a well-known public time server,
// so the clock can still sync on a network where hostname resolution is broken.
#define NTP_SERVER_1      "pool.ntp.org"
#define NTP_SERVER_2      "time.google.com"
#define NTP_SERVER_3      "216.239.35.0"      // time.google.com (anycast IP)

// Give up waiting for a sync after this long and just report "unsynced". The
// clock keeps retrying in the background; this only governs the UI label.
#define NTP_SYNC_TIMEOUT_MS  15000

// ---------------------------------------------------------------------------
// Layout constants for the new production UI
// ---------------------------------------------------------------------------
// Everything is designed against 480x272 with no scrolling.

// Shared top status bar (wifi icon + slash + SSID), drawn on every screen except
// the boot log. Replaces the old 30 px decorative header.
#define UI_STATUS_BAR_H      26
#define UI_STATUS_BAR_BG     0x1B1B28
#define UI_STATUS_BAR_FG     0xE8E8F0

// Analog clock on the standby screen.
#define UI_CLOCK_DIAMETER    150
#define UI_CLOCK_X           24
#define UI_CLOCK_Y           58

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
// 1 = print the periodic heartbeat, touch polls, I2C scans and panel stats.
// Verbose on purpose: the freeze we are chasing is only visible in the log.
#define DIAG_VERBOSE         1

// How often the main loop prints a heartbeat + touch snapshot (ms).
#define DIAG_HEARTBEAT_MS    2000

// ---------------------------------------------------------------------------
// Panel refresh strategy — the hardware-sensitive knob (select ONE)
// ---------------------------------------------------------------------------
// The RGB panel must be fed continuously, and LVGL must be able to redraw
// without the two fighting over the same memory. There are three ways to do
// this on the ESP32-S3 LCD_CAM; each is a real, observed behaviour on this
// board, not a guess.
//
//   1 (DEFAULT) DOUBLE-BUFFERED, FREE-RUNNING — `num_fbs = 2`.
//       The driver keeps two screen-sized PSRAM frame buffers and alternates
//       which one the LCD_CAM scans. `draw_bitmap()` writes the *other* one, so
//       a redraw can never appear on the glass half-written. This is the correct
//       fix for the every-few-seconds glitching seen with a single buffer, and
//       it keeps the vendor's proven free-running timing.
//       Cost: 2 x 261 KB of PSRAM (plenty available: 8 MB total).
//
//   2  SINGLE BUFFER, FREE-RUNNING — `num_fbs = 1`.
//       What Arduino_GFX's auto_flush effectively does, and the smallest memory
//       footprint. Known drawback: the LCD_CAM scans the same buffer LVGL is
//       writing, so a partial redraw visibly tears/glitches. Kept selectable
//       only to demonstrate the difference.
//
//   3  STAGED — DRAM bounce buffers + refresh_on_demand.
//       Lowest PSRAM bandwidth pressure, and the panel only refreshes when the
//       host hands over a frame. The project's own earlier notes record this
//       combination DEADLOCKING on this board ("the panel went black after the
//       first partial update"), and it is the configuration that came up solid
//       white on the first flash of this rewrite (see docs/STATE_AND_SPECS.md
//       section 6.5). It is kept for A/B testing, NOT as a default.
//
// The firmware is written so none of the three can deadlock: the flush release
// is always bounded by a timeout, whatever the mode.
#define LCD_REFRESH_MODE   1

#define LCD_MODE_DOUBLE_BUFFER_FREERUN  1
#define LCD_MODE_SINGLE_BUFFER_FREERUN  2
#define LCD_MODE_STAGED_BOUNCE          3

// --- Bounce buffers: independent of the frame-buffer count -------------------
//
// These were previously bundled into the mode, which was a mistake: the number
// of FRAME buffers solves tearing (two buffers, so a redraw never lands on the
// one being scanned) while BOUNCE buffers solve something completely different
// — DMA bandwidth starvation.
//
// Without bounce buffers the RGB DMA reads the frame buffer straight out of
// PSRAM, and it must sustain that continuously (~130 KB per frame, several MB/s)
// for the panel to have something to scan on every line. That is fine while the
// CPU is idle, but as soon as Wi-Fi is active the Wi-Fi stack competes for the
// same PSRAM bandwidth, the DMA misses its deadline, and the panel is fed stale
// or partially-updated lines. Observed symptom: the screen flashes/glitches, but
// ONLY once the web server is up and Wi-Fi is carrying traffic — and with
// flush+0 in the heartbeat, proving LVGL is not involved at all.
//
// Bounce buffers fix exactly that: the driver stages each block through a small
// buffer in INTERNAL DRAM, which the DMA can always read at full speed, so PSRAM
// contention cannot starve the panel.
//
//      0  = no bounce buffers (DMA reads PSRAM directly; glitches under Wi-Fi load)
//      34 = 68 KB total in internal DRAM; 8 refills per frame
//
// Must divide the frame buffer exactly (272 is divisible by 34): esp_lcd rejects
// the panel with "frame buffer size must be multiple of bounce buffer size"
// otherwise.
#define LCD_BOUNCE_LINES   34

// Convert the user-facing mode into the bundle of separate flags the driver and
// LVGL glue actually need, once, in one place.
//
// NOTE: bounce buffers are NOT part of the mode any more — they are controlled
// by LCD_BOUNCE_LINES above, because they fix a different problem (DMA bandwidth
// starvation under Wi-Fi load) from the frame-buffer count (tearing).
#if LCD_REFRESH_MODE == LCD_MODE_DOUBLE_BUFFER_FREERUN
  #define LCD_CFG_NUM_FBS           2
  #define LCD_CFG_REFRESH_ON_DEMAND 0
  #define LCD_MODE_NAME             "double-buffer free-running"
#elif LCD_REFRESH_MODE == LCD_MODE_SINGLE_BUFFER_FREERUN
  #define LCD_CFG_NUM_FBS           1
  #define LCD_CFG_REFRESH_ON_DEMAND 0
  #define LCD_MODE_NAME             "single-buffer free-running"
#elif LCD_REFRESH_MODE == LCD_MODE_STAGED_BOUNCE
  #define LCD_CFG_NUM_FBS           2
  #define LCD_CFG_REFRESH_ON_DEMAND 1
  #define LCD_MODE_NAME             "double-buffer on-demand"
#else
  #error "LCD_REFRESH_MODE must be 1, 2 or 3"
#endif

// Bounce buffers still need to divide the frame buffer exactly, whatever the
// mode, so validate the value here rather than only inside display_driver.cpp.
#if LCD_BOUNCE_LINES > 0
  #if (LCD_HEIGHT % LCD_BOUNCE_LINES) != 0
    #error "LCD_BOUNCE_LINES must divide LCD_HEIGHT (272) exactly"
  #endif
#endif
