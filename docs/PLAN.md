# Indorep QRIS Mini Display — ESP32-S3 Firmware Plan

Firmware for the physical **QRIS mini display** that shows the active QRIS payment
to customers. This device replaces the placeholder display that the Go backend
(`indorep_backend_v2`) already talks to at `http://192.168.4.254/`.

---

## 1. Contract with the Go backend (ground truth)

Source: `indorep_backend_v2/display/display.go` and `indorep_backend_v2/main.go`.

The backend treats the display as an **HTTP server** at a fixed URL
`http://192.168.4.254/`. The ESP32 must **host** two endpoints:

### `POST /update`
Content-Type: `application/json`. Body:

```json
{
  "qr": "00020101021226...",        // raw EMVCo QRIS string. MAY be empty.
  "cart": [ { "qty": 1, "item": "Kopi Susu" } ],
  "total_price": "Rp 15 000"        // already formatted by the backend
}
```

- `qr` empty (e.g. cash flow) => render **summary only** (no QR).
- `cart` and `total_price` may be omitted/empty => only the QR is meaningful.
- Must respond `200` quickly (backend client timeout is **5 s**).
- Backend examples: `createMember` (member + paket note), `topUpMember`
  (member + note), fridge/item orders (`item` + `qty` per menu), `/displayQris`
  (POS-driven active dialog).

### `GET /reset`
Clears the QRIS page and returns the device to the **standby home screen**.
Must respond `200`.

The backend's own `/reset` HTTP route (`main.go`) maps to `Display.Reset()`, and
`/displayQris` maps to `Display.OnScreen(...)`. No backend changes are required
as long as this firmware binds `192.168.4.254:80`.

---

## 2. Hardware

Authoritative source: the vendor folder shipped with the module,
`4.3inch_ESP32-4827S043/`. Pin and timing values in `include/board_config.h`
are transcribed from those demos, not guessed.

| Item      | Value                                                        |
|-----------|--------------------------------------------------------------|
| Module    | ESP32-4827S043 (ESP32-S3-WROOM-1-N16R8)                      |
| MCU       | ESP32-S3 (QFN56) v0.2                                        |
| Flash     | 16 MB                                                        |
| PSRAM     | 8 MB (OPI)                                                   |
| Panel     | 4.3" 480x272, driver **ILI6485**, 16-bit parallel **RGB**    |
| Panel bus | ESP32-S3 **LCD_CAM** peripheral (RPI-DPI), not SPI           |
| Touch     | **GT911**, I2C **SDA 19 / SCL 20**, RST **38**, INT unwired  |
| Backlight | plain **GPIO 2**, active high                                |
| Expander  | **none** (this module has no CH422G)                         |

References:
- `1-Demo/Demo_Arduino/3_3-1_TFT_HelloWorld/HelloWorld/HelloWorld.ino` (RGB pins + timings)
- `1-Demo/Demo_Arduino/3_3-4_TFT-LVGL-Widgets/LvglWidgets/touch.h` (touch pins + axis map)
- `1-Demo/Demo_Arduino/Libraries/Gt911-arduino-main` (vendored GT911 driver, used as-is)

A 480x272 parallel RGB panel cannot be driven by bit-banged SPI drivers
(TFT_eSPI). It needs the ESP32-S3 `LCD_CAM` peripheral via `esp_lcd`.

### Two platform constraints

1. **No USB-CDC.** GPIO19/20 carry the GT911 I2C bus, so native USB is not
   available. The console runs over the on-board USB-UART bridge (UART0,
   GPIO43/44).
2. **Arduino core 3.x required.** The stock PlatformIO `espressif32` platform is
   pinned to Arduino core 2.x / IDF 4.4, whose `esp_lcd` RGB API lacks the
   per-pin GPIO fields used here. The project uses the **pioarduino** fork
   (Arduino core 3.3.12 / IDF 5.5.5).


---

## 3. Stack

- **PlatformIO** project, `framework = arduino`, platform = **pioarduino**
  fork (`platform-espressif32`, Arduino core 3.x / IDF 5.x).
- **esp_lcd RGB panel** driver on the ESP32-S3 `LCD_CAM` peripheral, with
  DRAM bounce buffers and LVGL draw buffers in PSRAM.
- **LVGL 8.4** for the UI (`lv_conf.h` at the project root).
- **TAMC_GT911** (vendor-supplied, vendored via `symlink://`) for touch.
- **QRCode** (LVGL's built-in `lv_qrcode` / qrcodegen) to encode the QRIS
  string **on-device**.
- **ArduinoJson** for `/update` parsing.
- Arduino `WiFi` (station mode) + `WebServer`.

### Memory
- `board_build.arduino.memory_type = qio_opi`.
- `-DBOARD_HAS_PSRAM`.
- LVGL draw buffers and the QR canvas in **PSRAM**; RGB frame/bounce buffers
  via the esp_lcd driver.

### Do not enable USB-CDC
GPIO19/20 are the GT911 I2C bus on this module; native USB is unavailable and
the console uses the on-board USB-UART bridge (UART0, GPIO43/44).

---

## 4. Directory layout

```
esp32-screen-indorep/
├─ platformio.ini              # pioarduino platform, PSRAM, LVGL flags, lib deps
├─ lv_conf.h                   # LVGL v8.4 config (root, so the lib can find it)
├─ include/
│  ├─ board_config.h           # RGB pins/timing, GT911 pins, net config
│  └─ wifi_profiles.h          # the 3 APs (WARNET / CAFE / IDRP)
├─ src/
│  ├─ main.cpp                 # setup/loop: board init + screen state machine
│  ├─ display_driver.cpp/.h    # esp_lcd RGB panel (ILI6485) + LVGL draw buffers
│  ├─ touch_driver.cpp/.h      # GT911 (TAMC_GT911 lib) + axis mapping
│  ├─ net.cpp/.h               # Wi-Fi station connect + status
│  ├─ web_server.cpp/.h        # /update, /reset            (M2)
│  ├─ qr_render.cpp/.h         # QR string -> LVGL canvas   (M2)
│  └─ ui_screens.cpp/.h        # boot menu / confirm / home / qris  (M1)
├─ 4.3inch_ESP32-4827S043/     # vendor spec, demos, schematics (source of truth)
└─ docs/
   ├─ PLAN.md                  # this file
   └─ BRINGUP.md               # debug firmware: how to flash + verify
```

---

## 5. Screen state machine (product flow)

```
        +-------------------------------+
        |  BOOT_MENU                    |
        |  [ WARNET ]   [ CAFE ]        |
        |             debug             |
        +------+----------+-------------+
               |          |
   WARNET      |          |  CAFE            debug
   "INDOREP WARNET"   "INDOREP CAFE"     "IDRP" + pw
   (open)             (open)             (WPA2 topupgamemurah)
               |          |
               v          v
        +-------------------------------+
        |  CONNECTING  (spinner + SSID) |
        +---------------+---------------+
                        | associate -> DHCP -> start web server
                        v
        +--------------------------------------------+
        |  CONFIRM                                   |
        |   Connected SSID : <ssid>                  |
        |   IP address     : <ip>                    |
        |   Web server     : RUNNING :80 / FAILED    |
        |   [ Back ]                [ Finish ]       |
        +---------+----------------------+-----------+
              Back |                      | Finish
                   v                      v
            BOOT_MENU            +---------------------+
                                 |  HOMESCREEN (standby)|
                                 |  static "ready" text |
                                 +----------+----------+
                                            ^
                  POST /update non-empty qr |
                                            v
                                 +---------------------+
                                 |  QRIS PAGE          |
                                 |  QR + cart summary  |
                                 |  + total_price      |
                                 +----------+----------+
                                            |
                              GET /reset  --+--> HOMESCREEN
```

- **HOMESCREEN** is a placeholder for now (static text indicating the device is
  on and idle).
- **QRIS PAGE** shows the QR image and the item summary (`item x qty` lines from
  `cart`) plus `total_price`.
- `POST /update` with an **empty `qr`** shows the summary-only layout.

---

## 6. Build order (milestones)

1. **M0 — Debug bring-up (THIS STEP)**: boot, RGB screen, GT911 touch, Wi-Fi.
   A minimal firmware that proves the panel lights up, LVGL renders, touch
   coordinates are correct, and each of the 3 APs associates. See
   `docs/BRINGUP.md`.
2. **M1 — Full UI**: boot menu, confirm, home, QRIS screens wired into the state
   machine.
3. **M2 — Web server**: `/update` + `/reset`, JSON parsing, LVGL-thread-safe
   hand-off, QR rendering.
4. **M3 — Hardening**: static IP `192.168.4.254`, reconnection/watchdog,
   `/health` endpoint, serial diagnostics.

---

## 7. Networking notes

- Credentials are compile-time constants in `include/wifi_profiles.h`.
- The Go backend hardcodes `192.168.4.254`, so the shipped firmware should
  **force a static IP 192.168.4.254 / 255.255.255.0 / gw 192.168.4.1** on the
  chosen AP. During bring-up DHCP is used and the assigned IP is shown on the
  CONFIRM screen and serial log.
- Ports: HTTP on **80**.

---

## 8. Verification

- `pio run` compiles clean for `esp32-s3-devkitc-1`.
- Serial monitor shows: panel init, touch init, Wi-Fi associate + IP.
- Backend smoke test (once deployed):
  ```bash
  curl -X POST http://192.168.4.254/update \
    -H "Content-Type: application/json" \
    -d '{"qr":"00020101021226...","cart":[{"qty":1,"item":"Kopi Susu"}],"total_price":"Rp 15 000"}'
  curl http://192.168.4.254/reset
  ```
