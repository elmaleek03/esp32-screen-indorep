# Indorep QRIS Mini Display

Firmware for a 4.3" ESP32-S3 counter display that shows the active QRIS payment
(QR code + cart summary + total) to a customer. It is a small HTTP server: the
Indorep POS backend pushes a payment to it, and it renders the result on the
panel.

## Hardware

| | |
|---|---|
| Module | ESP32-4827S043 (ESP32-S3-WROOM-1-N16R8, 16 MB flash, 8 MB PSRAM) |
| Panel | 4.3" 480x272, driver ILI6485, 16-bit parallel RGB (ESP32-S3 LCD_CAM) |
| Touch | GT911 capacitive, I2C (SDA 19, SCL 20, RST 38, INT not wired) |
| Backlight | plain GPIO 2, active high |
| I/O expander | none on this module |

Two constraints follow from the wiring and are not optional:

- **No USB-CDC.** GPIO 19/20 are the touch I2C bus, so native USB is
  unavailable. The console runs over the on-board USB-UART bridge (UART0).
- **Arduino core 3.x.** The stock PlatformIO `espressif32` platform is stuck on
  Arduino core 2.x / IDF 4.4, whose `esp_lcd` RGB API lacks the fields used here.
  The project uses the pioarduino fork (Arduino core 3.x / IDF 5.x).

Pin numbers, RGB timings and polarity are in `include/board_config.h`, transcribed
from the vendor folder `4.3inch_ESP32-4827S043/`. That folder is the hardware
reference; do not invent values not found there.

## Panel and touch specifications

These are the values that actually work on this module. They were the hard part of
bring-up: the panel timings were buried in Arduino_GFX's
`Arduino_RPi_DPI_RGBPanel(...)` call in the vendor demo, and the GT911 needs its
register map handled exactly right. Both are reproduced here so they do not have
to be rediscovered.

### RGB panel timings (ILI6485, 480x272, 16-bit)

From `Arduino_RPi_DPI_RGBPanel(...)` in
`1-Demo/.../3_3-1_TFT_HelloWorld/HelloWorld.ino`:

| Parameter | Value |
|---|---|
| Resolution | 480 x 272 |
| Pixel clock | 9 MHz (`LCD_PCLK_HZ = 9000000`) |
| HSYNC pulse width | 4 |
| HSYNC back porch | 43 |
| HSYNC front porch | 8 |
| VSYNC pulse width | 4 |
| VSYNC back porch | 12 |
| VSYNC front porch | 8 |
| Pixel clock polarity | active negative (`pclk_active_neg = 1`) |
| Bus width | 16-bit RGB565 parallel |
| DMA burst size | 64 |

**Polarity caveat (this is where it goes wrong).** Arduino_GFX maps its
`hsync_polarity` argument to esp_lcd's `hsync_idle_low` flag *inverted*:
`hsync_idle_low = (hsync_polarity == 0) ? 1 : 0`. The vendor's `0` therefore means
the signal idles LOW, i.e. `idle_low = 1`. `board_config.h` keeps both as
"idle low" booleans (`LCD_HSYNC_IDLE_LOW = 1`, `LCD_VSYNC_IDLE_LOW = 1`), matching
`Arduino_ESP32RGBPanel::getFrameBuffer()`. Setting these the other way produced a
quarter-width image.

### RGB bus pins

Control and sync:

| Signal | GPIO |
|---|---|
| DE | 40 |
| VSYNC | 41 |
| HSYNC | 39 |
| PCLK | 42 |

Red, green and blue data lines:

| Signal | GPIO | Signal | GPIO | Signal | GPIO |
|---|---|---|---|---|---|
| R0 | 45 | G0 | 5 | B0 | 8 |
| R1 | 48 | G1 | 6 | B1 | 3 |
| R2 | 47 | G2 | 7 | B2 | 46 |
| R3 | 21 | G3 | 15 | B3 | 9 |
| R4 | 14 | G4 | 16 | B4 | 1 |
| | | G5 | 4 | | |

Data lines map least-significant-bit first: B0..B4 to `data_gpio_nums[0..4]`,
G0..G5 to `[5..10]`, R0..R4 to `[11..15]`. RGB565 bit order is
`bits[4:0]=blue, bits[10:5]=green, bits[15:11]=red`.

### Backlight

Plain GPIO 2, active high, driven on/off by default. There is no dedicated
brightness line; PWM dimming is available behind `LCD_BL_USE_PWM` but is not the
default, because on this board it left the panel dark. See `board_config.h`.

### Touch (GT911)

| Parameter | Value |
|---|---|
| Controller | GT911 capacitive, 5 points |
| I2C bus | SDA 19, SCL 20 |
| I2C reset | GPIO 38 |
| I2C interrupt | not wired (`PIN_TOUCH_INT = -1`) |
| I2C clock | 400 kHz |
| I2C address | 0x5D (primary) or 0x14 (secondary); both are probed |
| Raw range | X 0..479, Y 0..271 (matches the glass on this unit) |
| Axis transform | none (FLIP_X 0, FLIP_Y 0, SWAP_XY 0) |

The address is normally latched from the INT pin level during reset release, but
INT is not wired here, so the part powers up on whatever its own pull resistors
strap. The driver probes both 0x5D and 0x14 and uses whichever answers.

Register-level details the driver depends on (in `gt911.cpp`):

- `0x8040` command (write 1 = soft reset, 2 = re-read config)
- `0x8047` config block, 186 bytes (`0x8047`..`0x8100`), so the checksum byte and
  the fresh flag are both inside the buffer
- `0x8048`/`0x804A` X/Y output max (low then high byte), `0x804C` touch count
- `0x80FF` config checksum (the 184-byte sum plus this byte must be 0 mod 256)
- `0x8100` config fresh flag (write 1 to make the controller latch the config)
- `0x8140` product id (4 ASCII bytes, `"911\0"`), `0x8144` firmware version
- `0x814E` point info (bit 7 = data ready, bits[3:0] = point count)
- `0x814F` point 1 (7 bytes: id, x_lo, x_hi, y_lo, y_hi, size_lo, size_hi)

Two gotchas: point data starts at `0x814F`, **not** `0x8150` (using 0x8150 shifts
every coordinate); and the config read must cover 186 bytes from `0x8047` so the
checksum is included when recomputing it.

The active config this board accepts is `xmax=480 ymax=272 touch=5 cksum=0x9F`.

## Build and flash

```powershell
pio run                 # build
pio run -t upload       # flash (or --upload-port COMx)
pio device monitor -b 115200
```

If upload stalls at `Connecting...`: hold BOOT, tap RESET, release BOOT, then
upload. See `docs/BRINGUP.md` for the full bring-up and diagnostic guide.

## HTTP API

The device listens on port 80. The billing app posts to `/displayQris`, which
the backend maps onto `/update`; both shapes are the same.

### `POST /update`

```json
{
  "qr": "00020101021226...",
  "total_price": "Rp 15.000",
  "cart": [
    { "item": "ujang - Paket Bocah", "qty": 1, "price": 7000 },
    { "item": "Kopi Susu",           "qty": 2, "price": 1500 }
  ]
}
```

- `qr` is the raw EMVCo QRIS string. Empty means "summary only" (e.g. cash): the
  QR is hidden and the cart and total are still shown.
- `total_price` is a display string, printed verbatim. The device does not parse
  or reformat it.
- `cart[].item` is printed as-is on the item's second line. `qty` is the
  quantity, `price` the per-unit price; the first line renders `Nx  Rp <price>`
  with dot thousands separators. If `price` is omitted the line shows just `Nx`.
- The request must be answered within 5 s (the backend's timeout). The handler
  parses, copies into a mailbox and replies before any drawing happens.

### `GET /reset`

Returns the UI to the standby screen. Idempotent.

### `GET /` and `GET /health`

A JSON status page (device name, firmware tag, MAC, SSID, IP, RSSI, uptime, and
the address the backend is configured to call). Not used by the backend; useful
for "is the device up?" from a shell.

## Networking

The device takes its address from **DHCP** and reports it on the connected
screen, in the boot log, and on `/health`. There is no static configuration.

The backend hardcodes `192.168.4.254`, so for it to reach the device one of these
must hold: reserve that address for this device's MAC, repoint the backend at the
device's DHCP address, or bridge the two with a router `dst-nat` rule. The device
cannot solve this from its side. Details in `docs/BRINGUP.md` section 11.

Wi-Fi credentials for the three boot-menu choices are compile-time constants in
`include/wifi_profiles.h`.

## Screen flow

```
BOOT_SCREEN -> WIFI_SELECT -> CONNECTING -> SUMMARY -> HOME <-> QRIS
```

- **WIFI_SELECT**: WARNET and CAFE (open), plus a small Debug button (IDRP).
- **CONNECTING**: association with a Cancel button. On failure it returns to the
  chooser with the reason in a banner.
- **SUMMARY**: green check, SSID, the DHCP-assigned IP, and server state.
  Pressing Lanjut latches the route: WIFI_SELECT is unreachable for the rest of
  the boot. This is deliberate; the device is installed once per branch.
- **HOME**: standby analog clock (NTP) and branch name.
- **QRIS**: shows the QR, up to five cart items, and the total. It is the only
  light-themed (white) screen, because it faces the customer. `POST /update`
  moves it here; `GET /reset` returns it to HOME.

## Layout of the code

```
include/
  board_config.h     hardware pins/timing, network config, layout constants
  wifi_profiles.h    the three AP credentials
  app_state.h        shared types: Screen, PaymentPayload, PaymentItem
src/
  main.cpp           setup/loop, screen state machine, serial console
  display_driver.*   ILI6485 RGB panel + LVGL integration (flush path)
  touch_driver.*     GT911 -> LVGL glue, single I2C reader
  gt911.*            register-level GT911 driver
  gt911_i2c.*        I2C bus bring-up and scan
  net.*              Wi-Fi station, IP reporting, NTP
  web_server.*       /update, /reset, /health, payload mailbox
  qr_render.*        QRIS string -> LVGL QR canvas
  ui_screens.*       all screens and transitions
4.3inch_ESP32-4827S043/   vendor demos and schematics (read-only reference)
docs/                     PLAN.md, BRINGUP.md, STATE_AND_SPECS.md
```

## Design rules

These are not style preferences. Each exists because breaking it caused a
specific failure in an earlier revision, and the code comments record which.

1. **One I2C reader.** `touch::poll()` is the only function that talks to the
   GT911. Multiple readers clock-stretched the bus and froze `loop()`.
2. **One LVGL task.** Every widget access happens in `loop()`. The web server
   never touches LVGL: it publishes a payload into a mailbox and returns, and the
   loop performs the screen transition between two `lv_timer_handler` calls.
3. **No blocking flush.** `flushCb()` hands the buffer to the panel and returns;
   `tick()` waits (bounded) for the DMA-done flag before releasing LVGL. Releasing
   early lets LVGL overwrite the draw buffer mid-copy, which tears the panel.
4. **Never write an unchanged string to a label.** `lv_label_set_text()` marks the
   widget dirty unconditionally, so a redundant write from the loop is a redraw
   storm that starves touch input. Every setter change-detects first.
5. **Release the touch press on every screen change.** `ui::show()` calls
   `touch::forceRelease()` before mutating the widget tree, or the new screen
   receives no input.

## Serial console

Open the monitor at 115200 and press a key:

```
h / ?  help
d      dump GT911 registers + panel/touch/i2c statistics
s      rescan the touch I2C bus
t      toggle per-touch-sample logging
c      toggle raw touch coordinate trace (calibration)
x      cycle touch axis transform (8 modes)
b      cycle backlight level
= / -  backlight +5% / -5%
w      scan Wi-Fi now (blocks a few seconds)
r      return the UI to standby
q      show a local test QRIS page (no network needed)
i      full system report
m      print the station MAC (for a DHCP reservation)
u      back to the Wi-Fi chooser (only before the route is latched)
```

## Testing without the backend

- Press `q` on the serial console: renders a local three-item test payload, no
  network involved.
- `curl` against the device (use its real IP from the connected screen):

```bash
curl -X POST http://192.168.4.252/update \
  -H "Content-Type: application/json" \
  -d '{"qr":"00020101021226...","total_price":"Rp 10.000","cart":[{"item":"ujang - Paket Bocah","qty":1,"price":7000},{"item":"Kopi Susu","qty":2,"price":1500}]}'

curl http://192.168.4.252/reset
```

`/update` is ignored until setup is complete (the route is latched), so finish
the on-screen setup first. The log prints `[app] payload ignored: setup not
complete` if a request arrives too early.

## Dependencies

- LVGL 8.4 (`lv_conf.h` at the project root)
- ArduinoJson 7 (`/update` parsing)
- Arduino `WiFi` and `WebServer`
- QR encoding uses LVGL's built-in `lv_qrcode` / qrcodegen; no network round-trip

The vendor TAMC_GT911 Arduino library is deliberately not used; `src/gt911.cpp`
is a self-contained replacement. See the note in `platformio.ini` for the reasons.
