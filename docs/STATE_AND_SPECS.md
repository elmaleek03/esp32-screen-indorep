# Indorep QRIS Mini Display - Project State & Hardware Specs

Last updated: 2026-10-05
Status: **Full rewrite landed. Builds clean (0 warnings). Touch proven working on
hardware; panel refresh mode corrected after the first flash came up white.**

See `docs/BRINGUP.md` for the flash/verify/diagnose guide.

---

## 1. What this device is

A 4.3" ESP32-S3 screen mounted in the store that shows the **active QRIS payment**
to the customer. The Go backend (`indorep_backend_v2`) already treats a physical
mini display as an HTTP server and pushes QR + cart summary to it. This firmware
is that display.

The M0 bring-up (`boot menu + colour bars + touch dot`) has been **replaced** by
the complete product flow, including the `M1` UI, `M2` web server and on-device
QR rendering. `M3` hardening is partially in: the device takes a DHCP address
and reports it on the connected screen (no static configuration).

---

## 2. Backend contract (authoritative)

Source: `indorep_backend_v2/display/display.go` and `indorep_backend_v2/main.go`.

The backend hardcodes the device URL:

```go
url = "http://192.168.4.254/"
```

and calls exactly two endpoints. The ESP32 must **host** them.

### `POST /update`

`Content-Type: application/json`

```json
{
  "qr": "00020101021226...",
  "cart": [ { "qty": 1, "item": "Kopi Susu" } ],
  "total_price": "Rp 15 000"
}
```

| Field | Notes |
|---|---|
| `qr` | Raw EMVCo QRIS string. **May be empty** (cash flow) meaning "show the summary only". |
| `cart` | Array of `{qty, item}`. May be empty. |
| `total_price` | Pre-formatted display string, e.g. `"Rp 15 000"`. |

- Must reply `200` **within 5 s**. The Go client sets `Timeout: 5 * time.Second`
  and a hung device previously blocked the payment callback, which is why the
  timeout exists.
- The handler replies **before** any drawing: it parses, copies into a mailbox,
  and returns. QR encoding and the panel refresh happen later in the main loop,
  so a slow render can never push the reply past 5 s. See section 6.3.
- Go struct field order on the wire is `qr`, `cart`, `total_price`.

### `GET /reset`

Clears the QRIS page and returns the device to the standby home screen. Reply `200`.

### `GET /health` (extra, not used by the backend)

Returns JSON status (`ssid`, `ip`, `rssi`, `touch`, `requests`, `uptime_s`). Also
served on `/`. Makes "is the device up?" a one-command question.

### Backend call sites

| Backend function | What it renders |
|---|---|
| `createMember` (paket malam) | `"Paket Malam" x1` |
| `createMember` (normal) | `"Member: <user>"` + paket note |
| `topUpMember` / `topUpMemberOnline` | `"Member: <user>"` + paket note |
| fridge/menu order | one `DisplayItem` per menu, `{item: name, qty}` |
| `POST /displayQris` (POS-driven) | the active dialog's cart + total |

`formatRupiah` in the backend produces `"Rp 15 000"` (space thousands separator),
so the firmware must **not** reformat `total_price`; render it as received.

---

## 3. Hardware (source of truth: the vendor folder)

Everything below is transcribed from `4.3inch_ESP32-4827S043/`, the official
vendor bundle that ships with the module. That folder, not guesswork, is the
reference.

| Item | Value |
|---|---|
| Module | **ESP32-4827S043** |
| MCU | ESP32-S3 (QFN56) rev v0.2, dual core + LP core, 240 MHz |
| Module part | ESP32-S3-WROOM-1-**N16R8** |
| Flash | 16 MB, quad (QIO), 3.3 V |
| PSRAM | 8 MB, embedded OPI (reports `AP_3v3`), `esp_pmem` |
| Wi-Fi MAC | `cc:ba:97:02:ba:d8` |
| Panel | 4.3" TN, **480x272**, driver **ILI6485**, 65K colour |
| Panel interface | **16-bit parallel RGB** via ESP32-S3 **LCD_CAM** (RPI-DPI) |
| Touch | **GT911** capacitive, 5-point, own I2C bus |
| Backlight | plain **GPIO 2**, active high |
| I/O expander | **none** (no CH422G on this module) |
| USB | CH340 USB-UART bridge; console on UART0 (GPIO43/44) |

### Why the ESP32-S3 LCD_CAM and not SPI

A 480x272 parallel RGB panel cannot be bit-banged over SPI. It is driven by the
S3's dedicated LCD_CAM peripheral. The vendor demo uses Arduino_GFX's
`Arduino_RPi_DPI_RGBPanel`, which wraps exactly this.

### RGB bus pins (from `HelloWorld.ino`)

| Signal | GPIO | Signal | GPIO | Signal | GPIO |
|---|---|---|---|---|---|
| DE | 40 | G0 | 5 | B0 | 8 |
| VSYNC | 41 | G1 | 6 | B1 | 3 |
| HSYNC | 39 | G2 | 7 | B2 | 46 |
| PCLK | 42 | G3 | 15 | B3 | 9 |
| R0 | 45 | G4 | 16 | B4 | 1 |
| R1 | 48 | G5 | 4 | | |
| R2 | 47 | | | | |
| R3 | 21 | | | | |
| R4 | 14 | | | | |

Backlight: **GPIO 2** (`#define TFT_BL 2`).

### RGB timing (from `Arduino_RPi_DPI_RGBPanel`)

```
pclk_hz          9 000 000     (prefer_speed = 9000000)
h_res            480
v_res            272
hsync_front_porch      8
hsync_pulse_width      4
hsync_back_porch      43
vsync_front_porch      8
vsync_pulse_width      4
vsync_back_porch      12
pclk_active_neg        1
auto_flush          true
```

### Touch pins (from `LvglWidgets/touch.h`)

| Signal | GPIO |
|---|---|
| SDA | **19** |
| SCL | **20** |
| INT | **not wired** (`-1`) |
| RST | **38** |

Axis mapping in the vendor demo maps **both axes inverted** onto the panel:

```c
#define TOUCH_MAP_X1 480
#define TOUCH_MAP_X2 0
#define TOUCH_MAP_Y1 272
#define TOUCH_MAP_Y2 0
```

### Critical timing/polarity trap (already hit once)

`Arduino_ESP32RGBPanel::getFrameBuffer()` maps the vendor's polarity argument to
esp_lcd's *idle* flag **inverted**:

```c
hsync_idle_low = (hsync_polarity == 0) ? 1 : 0;
vsync_idle_low = (vsync_polarity == 0) ? 1 : 0;
```

So the vendor's `hsync_polarity = 0` means `hsync_idle_low = 1`. Setting this
backwards produced a **quarter-width shifted image**. `board_config.h` stores
`LCD_HSYNC_IDLE_LOW 1` / `LCD_VSYNC_IDLE_LOW 1` and explains why.

### Other vendor settings esp_lcd needs

```c
clk_src          = LCD_CLK_SRC_PLL160M
data_width       = 16
bits_per_pixel   = 0        // 0 => defaults to data_width. Vendor leaves it 0.
disp_gpio_num    = -1 / GPIO_NUM_NC
flags.fb_in_psram      = 1
flags.disp_active_low  = 0
// no bounce_buffer_size_px, no refresh_on_demand
```

---

## 4. Toolchain

| Component | Version |
|---|---|
| PlatformIO Core | 6.2.0 (pioarduino) |
| Platform | `pioarduino/platform-espressif32` (stable zip) |
| Arduino core | **3.3.12** |
| ESP-IDF | **5.5.5** |
| Compiler | `xtensa-esp-elf` 14.2.0 |
| Board id | `esp32-s3-devkitc-1` |
| Flash size override | `16MB`, `default_16MB.csv` |
| Memory type | `qio_opi` |
| Monitor | 115200 |

**Why the pioarduino fork:** the stock PlatformIO `espressif32` platform is stuck
on Arduino core 2.x / IDF 4.4, whose `esp_lcd` RGB-panel API predates the
per-pin GPIO fields (`hsync_gpio_num`, `data_gpio_nums[]`) this firmware needs.

**Python/venv note:** pioarduino's venv must be Python **3.11** (`~/.platformio/penv`).
Scoop's Python 3.14 triggers a venv-recreation loop that fails with
`Failed to install Python dependencies`. If that happens, delete
`~/.platformio/penv` and reinstall `pioarduino==6.2.0` into a venv created by
`~/.platformio/python3/python.exe`.

---

## 5. Repository layout

```
esp32-screen-indorep/
  platformio.ini                 # pioarduino platform, PSRAM, LVGL flags, lib deps
  lv_conf.h                      # LVGL 8.4 config, trimmed (see section 7)
  include/
    board_config.h               # ALL hardware truth: pins, timing, net, diag
    wifi_profiles.h              # the 3 APs
    app_state.h                  # Screen / NetChoice / PaymentPayload
  src/
    main.cpp                     # boot order, UI state machine, serial console
    display_driver.cpp/.h        # esp_lcd RGB panel + LVGL display + self-test
    gt911_i2c.cpp/.h             # owns the touch I2C bus (pins, 400k, RX 256, timeout)
    gt911.cpp/.h                 # GT911 register-level driver (replaces TAMC_GT911)
    touch_driver.cpp/.h          # GT911 -> LVGL glue, axis map, single-reader rule
    ui_screens.cpp/.h            # boot menu / connecting / confirm / home / qris
    qr_render.cpp/.h             # QRIS string -> LVGL QR canvas
    web_server.cpp/.h            # /update, /reset, /health + payload mailbox
    net.cpp/.h                   # Wi-Fi station connect + status
  docs/
    PLAN.md                      # original plan + flow + milestones
    BRINGUP.md                   # flash/verify/diagnose guide (current)
    STATE_AND_SPECS.md           # this file
  4.3inch_ESP32-4827S043/        # VENDOR BUNDLE (read-only reference)
    1-Demo/Demo_Arduino/         # HelloWorld, LVGL widgets, wifi, etc.
      Libraries/                 # Arduino_GFX, Gt911-arduino, Lvgl, ESP32-audioI2S
    2-Specification/             # ESP32-4827S043 Specifications-EN.pdf
    4-Driver_IC_Data_Sheet/      # ILI6485 / ESP32-S3 datasheets
    5-Schematic/                 # ESP32-4827S043-1.png, WROOM-1 pinout
    6-User_Manual/               # Getting started 4.3 Inch.pdf
    8-Burn operation/
```

Vendor libraries available in-tree (not all used):
`Arduino_GFX-master`, `Gt911-arduino-main` (TAMC_GT911, **deliberately unused** —
see section 6.4), `Lvgl` (8.3.0-dev), `ESP32-audioI2S-master`.

---

## 6. Current implementation state

### 6.1 Module map

The firmware was rewritten from the M0 bring-up into a complete product build.
Module responsibilities and, more importantly, the reason each boundary exists:

| File | Responsibility | Why it is separate |
|---|---|---|
| `src/main.cpp` | Boot sequence, UI state machine, serial console, always-on diagnostics. **The only place the LVGL tree is touched.** | Enforces the single-LVGL-task rule (§6.3). |
| `src/display_driver.cpp/.h` | esp_lcd RGB panel, LVGL display registration, flush hand-off, panel self-test. | Owns the flush/refresh protocol (§6.2). |
| `src/gt911_i2c.cpp/.h` | Owns the touch I2C bus: pins, 400 kHz, 256-byte RX buffer, 50 ms timeout, bus scan, raw register access. | Makes bus configuration explicit and loggable, and provides a timeout so a stuck transaction cannot wedge `loop()`. |
| `src/gt911.cpp/.h` | Register-level GT911 driver: reset, dual-address identify, product-id check, config program, point read. | Replaces the vendor `TAMC_GT911` library, which has four blocking defects (§6.4). |
| `src/touch_driver.cpp/.h` | GT911 → LVGL glue, axis mapping, the single-reader rule, health counters. | The atomic unit that fixed the freeze. |
| `src/ui_screens.cpp/.h` | The five screens, drawn once at boot and shown/hidden. | Avoids allocating/freeing widgets during transitions. |
| `src/qr_render.cpp/.h` | QRIS string → LVGL `lv_qrcode` canvas. | Isolates the QR encoder and its LVGL-heap budget. |
| `src/web_server.cpp/.h` | `/update`, `/reset`, `/health`; payload mailbox. | Never touches LVGL; publishes and returns (§6.3). |
| `src/net.cpp/.h` | Station connect (DHCP), IP reporting, failure classification. | Keeps Wi-Fi policy out of the UI. |

Removed: `src/touch_bus.cpp/.h` (superseded by `gt911_i2c`).

### 6.2 Working ✅

| Area | Evidence |
|---|---|
| Boot | Banner, chip/flash/PSRAM report. PSRAM 8188 KB free / 8192 KB. Chip reports **rev2**. |
| Panel | ILI6485 480x272 initialises; **colour-bar self-test pushes a correct frame with no LVGL involved**; LVGL then renders the boot menu. |
| Backlight | GPIO2 driven high early, before the self-test, so the pattern is visible. |
| LVGL | 8.4.0, full-screen single PSRAM draw buffer (261 KB), default dark theme. |
| Touch | **GT911 identified on hardware**: `0x5D: product id "911"`, config committed (xmax=480 ymax=272, cksum=0x9F). A real menu tap was observed: `[ui] menu tap -> "IDRP"`. |
| Wi-Fi | Connects to the chosen AP; takes a **DHCP address** and reports it on the connected screen and `/health` (no static configuration). |
| Web server | `/update`, `/reset`, `/health` implemented with a non-blocking mailbox hand-off. |
| QR | On-device encoding via LVGL's `lv_qrcode`; 216x216 canvas (5840 B). Local test page available by pressing `q` in the console. |
| Build | Clean, **zero warnings**, ~16 s; 119 KB RAM (36.3 %), 1.20 MB flash (18.3 %). |

### 6.3 The three design rules (each learned from a failure)

1. **One I2C reader.** `touch::poll()` is the only function that talks to the
   GT911. The previous build had three readers — the LVGL indev callback, a
   main-loop readout, and a 500 ms register dump. The GT911 clock-stretches and
   does not tolerate overlapping transactions; SCL was held low and `loop()` wedged
   permanently, freezing the screen and all buttons. `touch::dumpRegisters()` is
   now read-only and deliberately does **not** clear `0x814E`, because clearing it
   is the acknowledgement of a real sample.

2. **One LVGL task.** LVGL v8 is not thread-safe and no mutex makes it so. Every
   widget access happens in `loop()`. The web server parses a request, copies the
   payload into a static mailbox, replies `200`, and returns; `main.cpp` consumes
   the mailbox between two `lv_timer_handler()` calls. This is also what keeps the
   `/update` reply inside the backend's 5 s timeout: no drawing happens in the
   request path.

3. **No blocking flush.** `flushCb()` arms its flag, starts the transfer, and
   returns immediately — it never calls `lv_disp_flush_ready()`. The panel's
   completion callback only sets a flag. `display::tick()` waits (bounded, 40 ms)
   in **task context** and then releases LVGL exactly once. The bound means a lost
   interrupt costs one torn frame, never a deadlock.

### 6.4 Why the vendor TAMC_GT911 library is not used

All four verified by reading its source, not guessed:

1. `read()` ends with an unconditional `writeByteData(GT911_POINT_INFO, 0)`,
   clearing the data-ready flag even when the controller had no new sample. On a
   clock-stretching part polled from a UI timer, that races the panel's own frame
   rate and can drop every real sample.
2. Every helper ignores its I2C return value, so a NAK is indistinguishable from
   a genuine zero. This is what made the original fault hard to read.
3. `reset()` unconditionally calls `setResolution(480, 272)`, and `readPoint()`
   then applies its own `x = width - x` / `y = height - y` inversion **on top of**
   the vendor demo's `map()` — producing a doubly-mirrored result.
4. It latches its I2C address from the INT pin level during reset. INT is not
   wired on this module.

`src/gt911.cpp` is a self-contained replacement. The vendor folder stays in-tree
as read-only hardware reference and is **unmodified**.

### 6.5 The white-screen fault (first flash after the rewrite)

The first flash of the rewrite booted correctly — GT911 identified, Wi-Fi scanned,
touch taps registered — but the panel stayed **solid white**.

Root cause: the panel was configured with `refresh_on_demand = 1` plus DRAM bounce
buffers. With `refresh_on_demand`, the LCD_CAM has **no frame to send until the
first `draw_bitmap()`**, so the panel free-runs against whatever the frame buffer
held at power-on — uninitialised PSRAM, which reads as `0xFFFF`, i.e. white.

Two corrections, both now in `src/display_driver.cpp`:

1. The panel is **primed** immediately after init with a full black frame, then
   the self-test pattern, then the first LVGL frame. (The vendor bus does a 1x1
   version of this for the same reason.)
2. The default refresh mode is now the **vendor-proven free-running** path
   (`LCD_USE_BOUNCE_BUFFERS 0` in `board_config.h`): no bounce buffers, no
   `refresh_on_demand`, the controller scans the PSRAM frame buffer continuously —
   exactly what Arduino_GFX's `auto_flush` does.

The staged mode (bounce buffers + `refresh_on_demand`) remains selectable via
`LCD_USE_BOUNCE_BUFFERS 1` so it can be A/B tested on hardware, because the
project's own earlier notes record that combination deadlocking on this board
("the panel went black after the first partial update"). It is opt-in for
measurement, **not** the default. The firmware is written so neither mode can
deadlock, because the flush release is always bounded by a timeout.

### 6.6 Wire-bus ordering

`gt911_i2c::begin()` owns the bus outright, so ordering is explicit and logged:

```cpp
Wire.setBufferSize(256);                       // GT911 config block is 186 bytes; default RX is 128
Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, TOUCH_I2C_FREQ);
Wire.setTimeOut(50);                           // cap a stuck transaction so loop() cannot wedge
```

`setBufferSize()` must precede `Wire.begin()` on this core, because `begin()`
allocates the buffers. This is why the old note in this document describing
"`touchbus::prepare()` must not call `Wire.begin()`" no longer applies: the bus is
no longer started by a third-party library.

---

## 7. LVGL configuration notes

`lv_conf.h` lives at the **project root** and is found via `-I.` +
`-DLV_CONF_INCLUDE_SIMPLE`.

It is deliberately explicit and trimmed. LVGL's template enables PNG, GIF, BMP,
SJPG, ffmpeg, freetype, tiny_ttf, rlottie, the SDL driver, every font (8–48 px)
and every widget. Every enabled feature is another `.c` file, and editing
`lv_conf.h` invalidates **all** of them. The trim cut the LVGL compile from ~193
files to a few dozen.

Kept: `LV_USE_QRCODE` (needed for QRIS), widgets `ARC` (the spinner's base class)
/ `BAR` / `BTN` / `CANVAS` / `IMG` / `LABEL` / `SPINNER`, fonts 14/20/32,
`FLEX` + `GRID`, default dark theme.

Performance flags taken from the vendor's own `lv_conf.h` and the module's
performance notes:

```c
#define LV_MEMCPY_MEMSET_STD 1
#define LV_ATTRIBUTE_FAST_MEM IRAM_ATTR
```

**Memory:** `LV_MEM_SIZE` is 64 KB (up from 48 KB). The 216x216 1-bit QR canvas
takes ~5.8 KB from the LVGL heap for the widget's lifetime, the encoder's scratch
buffers take another ~1.4 KB transiently, and the five screens plus label text
account for ~20 KB. `LV_USE_ASSERT_MALLOC 1` turns any shortfall into an
immediate logged failure rather than a silent NULL dereference.

Vendor ships LVGL **8.3.0-dev**; this project uses **8.4.0** from the registry.
Both are v8, API compatible.

Note: `src/qr_render.cpp` includes LVGL's vendored `qrcodegen.h` directly (to size
a QR before creating the widget), which needs an explicit include path to LVGL's
`src/extra/libs/qrcode`. That is set in `platformio.ini`.

---

## 8. Design decisions & their reasons

| Decision | Reason |
|---|---|
| esp_lcd RGB panel directly, not Arduino_GFX | LVGL needs a flush callback + PSRAM draw buffers; esp_lcd gives both while keeping the vendor's exact timing. |
| Full-screen single draw buffer (261 KB PSRAM) | A partial/band buffer showed only "a quarter of the screen" when a flush was lost. One full-screen buffer = one transfer, nothing to partially lose. |
| Free-running refresh by default, not `refresh_on_demand` | The vendor path is the one proven to light this panel. `refresh_on_demand` came up white because the controller has no frame until the first `draw_bitmap()` (see §6.5). |
| Bounce buffers opt-in (`LCD_USE_BOUNCE_BUFFERS`) | The project's own notes record bounce buffers + `refresh_on_demand` deadlocking here, so it is selectable for measurement rather than assumed. |
| Panel primed with a full black frame after init | Deterministic first frame; without it the panel shows uninitialised PSRAM (white). |
| Panel self-test (colour bars) before LVGL | Splits a blank screen into "panel bring-up" vs "LVGL/flush" in one glance, with correct channels, bit order and geometry all proven at once. |
| Async flush via completion callback + bounded `tick()` wait | Calling `lv_disp_flush_ready()` immediately let LVGL overwrite the buffer mid-transfer, shearing the image. The ISR only sets a flag; `tick()` releases in task context with a timeout. |
| No mutex around the LVGL tree | LVGL v8 is not thread-safe; a mutex would only hide the race. All widget access is confined to one task instead. |
| Own GT911 driver, not TAMC_GT911 | Four concrete defects, see §6.4. |
| Buttons keep the theme's default style (no `remove_style_all`) | `remove_style_all` also strips the pressed state, so a tap gave no visual feedback — actively misleading on a device whose headline symptom was "touch does not work". |
| Failure banner on the boot menu | A silent bounce back to the menu looks like the tap was never registered. |
| `-I.` for lv_conf.h | LVGL builds as a separate lib and needs the config on its include path. |

---

## 9. Milestones

| # | Scope | State |
|---|---|---|
| **M0** | Boot, RGB screen, touch, Wi-Fi bring-up | ✅ Screen (after the §6.5 fix), touch proven on hardware, Wi-Fi ✅ |
| **M1** | Real UI: boot menu → connecting → confirm → standby, QRIS page | ✅ implemented |
| **M2** | Web server `/update` + `/reset`, on-device QR rendering | ✅ implemented |
| **M3** | Address reporting, `/health`, diagnostics | ✅ DHCP + IP on connected screen + `/health` + diagnostics done; **automatic reconnection not implemented** (see §11) |

### Product flow (implemented)

```
BOOT_MENU
  [ WARNET ]  [ CAFE ]  [ debug ]
      |            |         |
 "INDOREP WARNET" "INDOREP-CAFE" "IDRP" / pw "topupgamemurah"
   (open)          (open)          (WPA2)
      \            |         /
       -> CONNECTING (spinner + SSID)
            -> CONFIRM: SSID, IP address, web-server status
                 [ Back ]        [ Finish ]
                    |                 |
              BOOT_MENU         HOME (standby, "SIAP", live status + uptime)
                                    ^
                   POST /update     |
                (qr + cart + total) |
                                    v
                              QRIS PAGE  -- GET /reset --> HOME
```

Extra behaviour beyond the original plan:

- Tapping the standby HOME screen returns to the boot menu, so the operator can
  change APs without power-cycling. The web API remains the normal path, but a
  physical escape hatch matters when the device is mounted in a shop.
- A failed association returns to the menu with a **red banner** naming the
  reason (`AP tidak ditemukan`, `koneksi ditolak / password salah`, etc.).

**Confirmed AP names (real scan):**
- `INDOREP WARNET` (open)
- `INDOREP-CAFE` (**hyphen**, open)
- `IDRP` + `topupgamemurah` — not present in the last scan, so the debug path
  correctly reports `AP tidak ditemukan`.

---

## 10. Build / flash / monitor

```powershell
cd C:\Users\Maleek\Personal\Work\esp32-screen-indorep
pio run                          # build (~16 s)
pio run -t upload                # flash (auto-detects; or --upload-port COMx)
pio device monitor -b 115200     # console
```

Port notes: the CH340 re-enumerates, so the COM number changes between plug-ins
(observed COM4 → COM7, with stale ghost entries). Check with `pio device list`.
`COM3` is Intel AMT, not the board.

If upload stalls at "Connecting...": hold **BOOT**, tap **RESET**, release
**BOOT**, then upload.

Full verification and troubleshooting: **`docs/BRINGUP.md`**.

### Known build gotchas

| Symptom | Cause / fix |
|---|---|
| `ARDUINO_USB_MODE redefined` | Board variant already defines it; do not pass it. |
| `hal/clk_tree_defs.h: No such file` | Correct header is `soc/clk_tree_defs.h`. |
| `No member named 'pin_hsync'` etc. | Arduino core 2.x API. Use the pioarduino platform. |
| `lv_conf.h: No such file` | Needs `-I.` + `-DLV_CONF_INCLUDE_SIMPLE`. |
| `qrcodegen.h: No such file` | Needs the LVGL qrcode include path (already in `platformio.ini`). |
| `WL_WRONG_PASSWORD was not declared` | ESP8266-ism; Arduino core 3.x `wl_status_t` has no such member. |
| `frame buffer size must be multiple of bounce buffer size` | Only when bounce buffers are enabled: the line count must divide 272. |
| `Failed to install Python dependencies` | Corrupt penv; see §4. |
| Every rebuild recompiles everything | A header changed. `lv_conf.h` invalidates all of LVGL; `board_config.h` invalidates all of `src/`. Unchanged rebuild is ~16 s. |
| `firmware.bin: The process cannot access the file` | A stale `pio`/`python` process still holds it. Kill it and rebuild. |

---

## 11. Open questions / risks

1. **Panel refresh mode is a judgement call, not a measurement.** The default is
   the vendor-proven free-running path, and the self-test now proves the bus
   independently. `LCD_USE_BOUNCE_BUFFERS 1` should still be A/B tested on real
   hardware to see which gives a cleaner image under Wi-Fi load.
2. **Free-running + single frame buffer can tear.** The controller scans the
   buffer while LVGL writes it. This is the vendor's own tradeoff (`auto_flush`).
   If tearing is visible in practice, the fix is `num_fbs = 2` + double buffering,
   not a return to `refresh_on_demand`.
3. **No automatic Wi-Fi reconnection.** A dropout leaves the device offline until
   an operator taps the menu or power-cycles. Deliberate for now — a silent
   reconnect loop hides failures — but it is a real gap for a shop device left
   running for days. This is the main outstanding M3 item.
4. **`total_price` is rendered verbatim**, as required. Correct, but it means a
   malformed string from the backend is displayed as-is. `PAYLOAD_TOTAL_MAX` is
   48 bytes and truncation is silent.
5. **QR payload capacity.** A QRIS string longer than the encoder can fit in the
   216 px canvas is reported (`QR GAGAL DIBUAT`) rather than shown stale, but it
   is not yet surfaced to the backend. Real QRIS strings are ~200-400 chars and
   fit comfortably; this is a guard, not a live concern.
6. **`MAX_DISPLAY_ITEMS` is 8.** A larger cart is truncated on screen (logged).
   Chosen to fit 480x272 without scrolling.
