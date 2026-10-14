# Bring-Up: flash, verify, diagnose

Firmware for the **ESP32-4827S043** 4.3" module (ESP32-S3, 480x272 ILI6485 RGB,
GT911 touch). This document covers the diagnostic build: what it prints, what to
look for, and how to localise a fault.

Hardware truth (pins, timing, polarity) lives in `include/board_config.h` and is
transcribed from the vendor folder `4.3inch_ESP32-4827S043/`. Never invent pin
numbers — that folder is the reference.

---

## 1. Build, flash, monitor

```powershell
cd C:\Users\Maleek\Personal\Work\esp32-screen-indorep
pio run                    # build
pio run -t upload          # flash (auto-detects; or --upload-port COMx)
pio device monitor -b 115200
```

If upload stalls at `Connecting...`: hold **BOOT**, tap **RESET**, release
**BOOT**, then upload. The CH340 re-enumerates, so the COM number changes between
plug-ins; `COM3` is Intel AMT, not the board.

---

## 2. What good boot output looks like

```
=============================================================
 INDOREP QRIS MINI DISPLAY - DIAGNOSTIC FIRMWARE (full rewrite)
 board: ESP32-4827S043  4.3in 480x272 ILI6485, GT911 touch
=============================================================
 chip   : ESP32-S3 rev2, 2 core(s) @ 240 MHz
 flash  : 16384 KB
 psram  : 8188 KB free / 8192 KB total      <- must be non-zero
 heap   : 221 KB free
[lcd] configuring RGB panel (ILI6485 480x272, 16-bit)
[lcd] panel ready: 480x272 @ 9000000 Hz, refresh=free-running (vendor), PSRAM draw buffer 255 KB
[lcd] priming panel with one black frame (130560 px)   <- mandatory; see §5
[lcd] selftest: pushing colour-bar pattern (no LVGL)
[i2c] bus up: sda=19 scl=20 freq=400000 rxbuf=256 timeout=50ms
[gt911] hw reset done (rst=38 held 12ms, settled 120ms)
[gt911] 0x5D: ACK, identifying...
[gt911] 0x5D: product id "911" (raw 39 31 31 00)        <- proof of life
[gt911] identified at 0x5D, product "911"
[gt911] config read ok: ver=0x41 xmax=480 ymax=272 touch=5 cksum=0x9F fresh=0
[gt911] config committed: xmax=480 ymax=272 touch=5 cksum=0x9F
[touch] controller ready at 0x5D, product "911"
[touch] LVGL indev registered (single-reader rule active)
[qr] canvas created: 216x216, 5840 bytes from the LVGL heap
[boot] UI built (boot menu)
[boot] first LVGL frame pushed (flushes=N)
[wifi] scanning...
[wifi]   INDOREP WARNET                 rssi= -26 ch= 8 enc=0
[wifi] menu APs present: WARNET=1 CAFE=1 IDRP=0
[boot] setup complete - running
```

**On screen, in order:**

1. **Colour bars + grey ramp + white-bordered black block**, for ~0.4 s. This is
   the panel self-test and it bypasses LVGL entirely.
2. The **boot menu**: three buttons (WARNET / CAFE / debug) and a touch-health
   line at the bottom.

If step 1 appears, the RGB bus, pins, timings and polarity are all correct.
If step 2 does not replace it, the fault is in LVGL or the flush path — see §5.

---

## 3. Serial console

Open the monitor and press a key:

| Key | Action |
|---|---|
| `h` / `?` | help |
| `d` | dump GT911 registers + panel/touch/i2c statistics |
| `s` | rescan the touch I2C bus |
| `t` | toggle per-touch-sample logging |
| `c` | toggle the RAW coordinate trace (raw -> mapped per sample) |
| `x` | cycle the touch axis transform (8 modes) |
| `b` | cycle the backlight 100/75/50/35/20/10/5/0% |
| `=` / `-` | backlight +5% / -5% |
| `w` | scan Wi-Fi now (blocks ~2-4 s; deliberately not done at boot) |
| `r` | return the UI to the standby homescreen |
| `q` | draw a local test QRIS page (no network required) |
| `m` | print the station MAC (for a router static lease) |
| `i` | full system report |

`q` is the fastest way to prove the QR path and the QRIS layout without the
backend or a Wi-Fi connection.

### Backlight and brightness

GPIO2 drives the whole backlight through a transistor — there is **no analog
brightness input**, so dimming is done by switching that line with PWM (LEDC at
5 kHz, 10-bit) and letting persistence of vision do the rest. Below roughly
200 Hz a backlight visibly flickers; 5 kHz is far above that and above the
audible band, so it is neither seen nor heard.

The startup level is `LCD_BL_START_PERCENT` in `board_config.h`, default **50%**:
full brightness is harsh for a counter-facing panel and is the largest power/heat
consumer in the device.

Tune it live rather than guessing, because "comfortable" is subjective — check it
on the actual counter:

- **`b`** cycles 100 / 75 / 50 / 35 / 20 / 10 / 5 / 0 %
- **`=`** and **`-`** adjust by 5% per press

Once you have picked a value, put it in `LCD_BL_START_PERCENT` so it survives a
reboot. The current level is reported by `i` and in the boot log.

If the PWM attach ever fails the firmware falls back to plain on/off and says so
in the log, so a PWM problem can never leave the panel dark and undiagnosable.

---

## 4. Interactive checks

| Action | Expected |
|---|---|
| Touch anywhere on the menu | button shows a lighter **pressed** state; serial logs `[ui] menu tap -> "<ssid>"` |
| Tap **WARNET** | confirms, then CONFIRM screen with SSID + IP + `RUNNING :80` |
| Tap **CAFE** | same for `INDOREP-CAFE` |
| Tap **debug** | tries `IDRP` WPA2. If it is not in range, the menu returns with a **red failure banner** naming the reason |
| Backend `POST /update` | QRIS page: QR on the right, cart + total on the left |
| Backend `GET /reset` | returns to the standby homescreen |
| `GET /health` in a browser | JSON status (ssid, ip, rssi, touch, uptime) |

The heartbeat prints every 2 s and is the fastest way to see whether the device
is alive and what it thinks is happening:

```
[hb] t=12s screen=3 wifi=INDOREP WARNET 192.168.4.254 rssi=-26 web=up flush=57/0 touch=12/3100 heap=113K psram=7665K
```

- `flush=N/0` — `N` flushes served, `0` forced after a timeout. **`forced` must
  stay 0**; a rising count means the panel is not completing transfers.
- `touch=S/P` — `S` samples seen, `P` polls issued. `S` only increases when a
  finger is down; `P` climbs continuously. If `P` climbs but `S` never does, the
  controller answers but reports nothing — see §6.

---

## 5. Blank / white screen — how the two cases are told apart

The self-test in step 1 exists precisely to split this question in two.

| Observation | Meaning | Where to look |
|---|---|---|
| **No colour bars at all**, screen white/black | Panel bring-up is wrong. LVGL is irrelevant. | §5.1 |
| **Colour bars appear**, but the menu never replaces them | Panel is fine; the flush path or LVGL is wrong. | §5.2 |
| Bars appear, menu appears, but colours are swapped (red↔blue) | `data_gpio_nums[]` bit order in `display_driver.cpp` | §5.3 |
| Bars appear but the image is shifted / wrapped / quarter-width | RGB blanking or polarity | §5.4 |

### 5.1 Panel bring-up faults

The most common one, and the reason the first flash of this rewrite came up
solid white: **with `refresh_on_demand` enabled the controller has no frame to
send until the first `draw_bitmap()`, so the panel free-runs against whatever the
frame buffer held at power-on** — uninitialised PSRAM reads as `0xFFFF`, which is
white. The fix is the two lines you see in the log:

```
[lcd] priming panel with one black frame (130560 px)
```

and the refresh mode:

```
refresh=free-running (vendor)
```

`LCD_USE_BOUNCE_BUFFERS 0` in `board_config.h` selects the vendor-proven
free-running mode. Set it to `1` to try the staged (bounce buffer +
refresh-on-demand) mode; the project notes record that combination deadlocking
on this board, so it is opt-in for A/B testing, not the default.

### 5.2 Flush-path faults

`[hb] ... flush+N(full N part N)` is the signal. Read it carefully, because it
tells you whether LVGL is involved at all:

- **`flush+0` while the screen is visibly glitching** ⇒ LVGL is NOT the cause.
  The fault is in the panel/DMA path — the controller is being fed stale or
  partial data. On this board that means **PSRAM bandwidth starvation**: without
  bounce buffers the RGB DMA reads the frame buffer straight from PSRAM and must
  sustain that continuously, so as soon as Wi-Fi competes for the same bandwidth
  the DMA misses deadlines and the image breaks up. The tell is that it starts
  exactly when `web=up` / Wi-Fi is carrying traffic.
  **Fix: `LCD_BOUNCE_LINES 34`** (already the default). That stages each block
  through internal DRAM, which the DMA always reads at full speed.
- **`partial` flooding while nothing changes on screen** ⇒ something is
  animating or a label is being rewritten in a loop. Check for an animated widget
  or a `lv_label_set_text()` on a hot path. The heartbeat's per-interval deltas
  make this obvious; a static screen should sit at `flush+0`.
- **`flush+N` and the UI feels sluggish** ⇒ lower `LV_DISP_DEF_REFR_PERIOD` is
  the wrong instinct; first check that no redundant `lv_label_set_text()` is
  running (see above).

Press `d` for the full picture:

```
[lcd] health: flushes=120 (full=3 partial=117) outstanding=0 last_area=8640 px
              scanning=0 internal_free=142 KB (largest 98 KB) psram_free=7665 KB
```

- `internal_free` below ~40 KB is a problem: the Wi-Fi stack and (if enabled)
  the bounce buffers live in internal DRAM. Bounce buffers cost
  2 x lines x 480 x 2 bytes = 63.8 KB at 34 lines.
- `maxwait` no longer exists: the flush is released immediately in the callback,
  which is safe because `num_fbs = 2` rotates two frame buffers.

### 5.3 Red/blue swapped

Fix the ordering of `cfg.data_gpio_nums[]` in `src/display_driver.cpp`. It is
indexed from the least significant bit: `[0..4]` = blue, `[5..10]` = green,
`[11..15]` = red.

### 5.4 Shifted / wrapped / quarter-width image

The vendor's `hsync_polarity = 0` maps to `hsync_idle_low = 1` in esp_lcd
(Arduino_GFX inverts the sense). `board_config.h` stores the already-corrected
booleans. If the image is still wrong, widen the blanking
(`LCD_HSYNC_*`, `LCD_VSYNC_*`) before touching `LCD_PCLK_HZ`.

---

## 6. Touch diagnostics

The GT911 is proven alive when this line appears:

```
[gt911] 0x5D: product id "911" (raw 39 31 31 00)
```

An I2C ACK alone is **not** accepted as identification — 0x8140 must read `911`.

### Calibrating a mirrored / rotated digitizer

The digitizer and the glass are separate parts and can be mounted rotated
relative to each other, so the raw coordinates do not have to increase toward the
panel's bottom-right. **Do not guess this from the config file.** Measure it.

There is deliberately **no on-screen calibration page** any more: it was needed
once, for this unit, and its buttons cluttered the boot menu with something an
operator in a shop should never touch. Calibration is a serial-console activity.

1. Press **`c`** to start the per-sample trace. It prints:

   ```
   [touch] raw=(478,3) -> mapped=(1,268)   [expect top-left raw~(0,0) mapped~(0,0)]
   ```

2. Touch each corner of the panel and compare. The raw pair at the panel's
   **top-left** tells you what is needed:

   | raw at panel top-left | transform |
   |---|---|
   | `(0,0)` | none |
   | `(max,0)` | flip X |
   | `(0,max)` | flip Y |
   | `(max,max)` | flip X and Y (180°) |
   | axes transposed | swap XY |

3. Instead of working it out by hand, press **`x`** to cycle the eight transforms
   live and re-touch until the reported `mapped` point matches your finger.

4. Write the winner into `TOUCH_FLIP_X` / `TOUCH_FLIP_Y` / `TOUCH_SWAP_XY` in
   `board_config.h` so it survives a reboot. As of this writing **`0,0,0`
   (normal) is correct for this unit** — the 180° error seen during bring-up was
   the transform being applied on top of a digitizer that already matched.

### Touch troubleshooting

| Symptom | Cause / fix |
|---|---|
| `no ACK` at both 0x5D and 0x14, then a bus scan with nothing on it | Wiring, or RST (GPIO38) not released. Also confirm USB-CDC is **off**: GPIO19/20 are the touch bus. |
| Address ACKs but product id is wrong | Drop `TOUCH_I2C_FREQ` to `100000` in `board_config.h` and retry. |
| Product id reads, `polls` climbs, `samples` never does | Nothing is registered as a touch. Press `d` and check `0x814E` while holding a finger: it should read `0x8x`. If it stays `0x00`, the panel's touch layer is not detecting the finger — a panel/hardware issue, not a driver one. |
| Coordinates mirrored or rotated | See "Calibrating" above (`c` to trace, `x` to cycle). |
| `forced` (releases) climbing on its own | The controller is not reporting release frames; the 700 ms watchdog is doing the work. Indicates a flaky bus — check `bus_errors`. |
| Touch works on the menu but **nothing works after navigating** | Fixed. Two causes: (a) the pressed state flapped because "no new sample" was treated as "released", and (b) a screen change left LVGL's indev holding a pointer to a widget on the hidden screen. `touch::forceRelease()` is now called on every transition in `ui::show()`. |
| Touch dead for a few seconds after boot | Fixed. The boot-time Wi-Fi scan blocked for 2-4 s with `display::tick()` never running. The scan is gone from boot; use the `w` console command when you want it. |
| Touch dead on the connecting screen | Fixed. `net::poll()` called `WiFi.status()`/`SSID()`/`gatewayIP()` on every loop pass, and each takes the IDF Wi-Fi driver mutex, which is held for long stretches during association — so `loop()` blocked before reaching `display::tick()`. Those reads are now rate-limited to 10 Hz, and `display::tick()` runs FIRST in the loop. |

### Why input used to die: the blocking chain

Three separate things used to starve `display::tick()` — and `display::tick()` is
what runs the LVGL timers, which is what polls touch. Anything that blocks in
front of it stops all input:

1. `WiFi.scanNetworks()` at boot (2-4 s).
2. `net::poll()`'s Wi-Fi accessors on every loop pass (mutex held by the Wi-Fi
   task during association).
3. `display::tick()` itself waiting up to 40 ms per flush for the panel's
   completion callback.

All three are gone: the boot scan is removed, the accessors are rate-limited, and
`display::tick()` no longer waits at all (safe because `num_fbs = 2` rotates two
frame buffers, so a redraw never lands on the buffer being scanned).

### Why the vendor library is not used

`TAMC_GT911` has four defects that make it unusable here, all verified by
reading its source:

1. `read()` clears the data-ready flag unconditionally, even when the controller
   had no new sample, so it races the panel's own frame rate.
2. It ignores every I2C return code, so a NAK is indistinguishable from a
   genuine zero — which is exactly what made the original fault hard to read.
3. `reset()` always re-programs resolution, and `readPoint()` applies its own
   axis inversion on top of the vendor demo's, producing a doubly-mirrored
   result.
4. It latches its I2C address from the INT pin, which is not wired on this
   module.

`src/gt911.cpp` is a self-contained register-level replacement; the vendor folder
stays in-tree as read-only hardware reference.

---

## 7. Memory

| Metric | Normal | Notes |
|---|---|---|
| `internal_free` (from `d`) | > 80 KB | Wi-Fi/LWIP live here. Below ~40 KB is trouble. |
| `psram_free` | ~7600 KB | The 261 KB LVGL draw buffer comes out of here. |
| Static RAM | 36.3 % | 119 KB of 320 KB. |
| Flash | 18.3 % | 1.20 MB of 6.5 MB. |

Internal DRAM is the tight resource, not PSRAM. Bounce buffers are the main
consumer when enabled (63.8 KB at 34 lines) — another reason the default mode is
the one without them.

---

## 8. Diagnostics design rules

Three rules exist here because breaking each one produced a specific, observed
failure:

1. **One I2C reader.** `touch::poll()` is the only function that talks to the
   GT911. The previous build had three readers (LVGL indev, a main-loop readout,
   and a 500 ms register dump); overlapping transactions held SCL low and wedged
   `loop()` permanently. `touch::dumpRegisters()` is now read-only and does not
   clear the status byte.
2. **One LVGL task.** Every widget access happens in `loop()`. The web server
   never touches widgets — it publishes a payload into a mailbox and `main.cpp`
   consumes it between two `lv_timer_handler()` calls.
3. **No blocking flush.** `flushCb()` returns immediately; `display::tick()`
   waits (bounded, 40 ms) for the panel's completion callback and then calls
   `lv_disp_flush_ready()`. The bound means a lost interrupt costs one torn
   frame, never a deadlock.

Touching any of these without understanding why it is there will reintroduce the
freeze.

---

## 9. HTTP API

The Go backend (`indorep_backend_v2`) hardcodes `http://192.168.4.254/` and calls
exactly two endpoints. Both must answer within the backend's 5 s timeout.

### `POST /update`

```json
{
  "qr": "00020101021226...",
  "cart": [ { "qty": 1, "item": "Kopi Susu" } ],
  "total_price": "Rp 15 000"
}
```

- `qr` may be empty (cash flow) → summary-only layout, QR hidden.
- `cart` and `total_price` may be omitted.
- The handler parses, copies into a mailbox, and replies `{"ok":true}` **before**
  any drawing. QR encoding and the panel refresh happen later in the main loop,
  so a slow render can never push the reply past 5 s.

### `GET /reset`

Clears the QRIS page and returns to standby. Replies `{"ok":true}`.

### `GET /health` (and `GET /`)

Not used by the backend; a one-command "is it up?" check:

```json
{"ok":true,"device":"indorep-qris-display","fw":"diag-1",
 "mac":"CC:BA:97:02:BA:D8","ssid":"INDOREP WARNET","ip":"192.168.4.252",
 "backend_addr":"192.168.4.254","same_as_backend":false,
 "rssi":-33,"touch":"ok","requests":4,"uptime_s":612}
```

`backend_addr` is where the Go backend is **configured** to knock, and
`same_as_backend` is a plain string comparison against this device's `ip`.

**Neither is a fault report.** The firmware cannot know whether the backend has
since been repointed at this device, nor whether another host forwards traffic
here, so it states the configured target and lets you reason about routing from
the backend side. A mismatched address is not by itself a problem — it just tells
you the two numbers differ.

### Compatibility with the device this replaces

Verified against the unit that was on `192.168.4.254`:

| Endpoint | Replaced device | This firmware |
|---|---|---|
| `POST /update` | `200 {"status":"ok"}` | `200 {"status":"ok"}` — **identical** |
| `GET /reset` | `200 {"status":"ok"}` | `200 {"status":"ok"}` — **identical** |
| `GET /` | `404` | `200` (extra, harmless) |
| `GET /health` | `404` | `200` (extra, harmless) |

The two endpoints the Go backend uses behave identically, **including the
response body format**, so a future backend change cannot tell the two apart.

**Response latency is the one real difference, and it is the point of the
rewrite:**

| | replaced device | this firmware |
|---|---|---|
| `GET /reset` | ~27 ms | ~5 ms |
| `POST /update` | **~250 ms** | ~5 ms |

The replaced unit did the QR encode and panel refresh *inside* the HTTP handler,
which is why its `POST` cost a fixed ~250 ms and could exceed the backend's 5 s
timeout under load — producing `context deadline exceeded` while awaiting
headers. This firmware replies first and draws afterwards, so the reply is never
gated on the display.

### Smoke test

```bash
curl -X POST http://192.168.4.254/update \
  -H "Content-Type: application/json" \
  -d '{"qr":"00020101021226...","cart":[{"qty":1,"item":"Kopi Susu"}],"total_price":"Rp 15 000"}'
curl http://192.168.4.254/reset
curl http://192.168.4.254/health
```

No network handy? Press **`q`** in the serial monitor to render the same payload
locally.

---

## 10. Known gotchas

| Symptom | Cause / fix |
|---|---|
| `ARDUINO_USB_MODE redefined` | Board variant already defines it; do not pass it. |
| `hal/clk_tree_defs.h: No such file` | Correct header is `soc/clk_tree_defs.h`. |
| `No member named 'pin_hsync'` | Arduino core 2.x API. Use the pioarduino platform. |
| `lv_conf.h: No such file` | Needs `-I.` + `-DLV_CONF_INCLUDE_SIMPLE`. |
| `qrcodegen.h: No such file` | Needs the LVGL qrcode include path (already set in `platformio.ini`). |
| `WL_WRONG_PASSWORD was not declared` | ESP8266-ism; core 3.x has no such enum member. |
| `frame buffer size must be multiple of bounce buffer size` | `bounce_buffer_size_px` must divide 480x272 exactly. Use a line count that divides 272 (1/2/4/8/16/17/34/68/136/272). |
| `Failed to install Python dependencies` | Corrupt penv; the pioarduino venv must be Python 3.11. |
| Every rebuild recompiles everything | A header changed. `lv_conf.h` invalidates all of LVGL; `board_config.h` invalidates all of `src/`. Unchanged rebuild is ~16 s. |
| `firmware.bin: The process cannot access the file` | A stale `pio`/`python` process still holds it. Kill it and rebuild. |

---

## 11. Address assignment, the backend, and MikroTik

### The constraint

The Go backend (`indorep_backend_v2/display/display.go`) hardcodes:

```go
url = "http://192.168.4.254/"
```

with `Timeout: 5 * time.Second`. It calls **only** that address. There is no
discovery and no fallback.

### How this firmware gets an address

**DHCP only.** The device runs no static configuration and claims no fixed
address: the router leases it an address and the device uses that address as-is.
The assigned address is shown on the "Terhubung!" (connected) screen, printed on
the serial boot log, and reported by `/health`.

This replaced an earlier DHCP-then-static-fallback scheme (`NET_IP_MODE 1`, which
forced `192.168.4.252`). That scheme did not work reliably on this network — the
fallback address was frequently already in use or outside the router's lease
pool, so the claim conflicted and the device became unreachable, the very failure
it was meant to prevent. DHCP is what works, so the static machinery is gone
(`NET_IP_MODE` is now `0`).

### The consequence for the backend, stated plainly

The Go backend calls **only** `192.168.4.254:80`. This device now sits on a
**DHCP-assigned** address, so unless something makes those two coincide, the
backend cannot reach it. The firmware deliberately does **not** report this as an
error: it shows its own IP and the address the backend expects, in muted grey,
and stops there. The device cannot know whether the backend has been repointed or
whether something forwards `.254:80` here, so it states the fact rather than
blaming itself for a network/routing problem it cannot see.

### How to make the backend reach the device — pick one

**Option A — reserve the address the backend already uses (preferred, no code
change).** Pin this device's MAC to `192.168.4.254` in the router's DHCP server,
so DHCP hands back the address the backend expects:

```
/ip dhcp-server lease
add address=192.168.4.254 mac-address=CC:BA:97:02:BA:D8 \
    comment="indorep qris display"
```

Get the MAC from the connected screen, the boot log, or by pressing `m` in the
serial console. This requires `.254` to be free — if another host holds it, that
host must be retired first (two hosts on one address breaks both).

**Option B — repoint the backend at the device's DHCP address (no router rule).**
Read the device's IP off the connected screen (or `/health`) and change the
backend's `url` to match. Simplest when you control the backend deployment, but
the address can change on the next lease unless you also reserve it.

**Option C — bridge the two addresses on the MikroTik (no reflash, no backend
change).** Destination-NAT everything the backend sends to `.254:80` over to
whatever the device's DHCP address is:

```
/ip firewall nat
add chain=dstnat protocol=tcp dst-address=192.168.4.254 dst-port=80 \
    action=dst-nat to-addresses=<device-dhcp-ip> to-ports=80 \
    comment="indorep qris display: backend .254 -> device DHCP addr"
```

The address in `to-addresses` must be kept in sync with the lease (reserve it),
so Option A is usually less to maintain.

### Checking it worked

From **inside the backend's network** (not from a machine on `192.168.4.x`):

```bash
curl -v --max-time 5 http://<device-ip>/health      # <device-ip> from the panel
```

```json
{"ok":true,"mac":"CC:BA:97:02:BA:D8","ip":"192.168.4.132",
 "backend_addr":"192.168.4.254","same_as_backend":false, ...}
```

What matters is whether **curl gets a response at all**. If it does, routing
between the backend's subnet and `192.168.4.0/24` is fine. If it times out, the
fault is inter-subnet routing (see the note below), not this device.

### Symptom: the backend's subnet cannot reach 192.168.4.x at all

If even the **router itself** times out pinging the display:

```
/ping <device-ip>             # from the MikroTik over SSH/Winbox
```

...then the device is not the problem and no firmware change will help. Check,
in this order:

1. `/ip address print detail` — is the `192.168.4.1/24` address present, on the
   correct interface, and not flagged invalid/disabled?
2. `/ip route print` — is there a reachable connected route for
   `192.168.4.0/24` via that interface? Without it the router cannot send
   anything into `192.168.4.x`.
3. `/ip firewall filter print stats` — a rule permitting
   `192.168.5.0/24 -> 192.168.4.0/24` on port 80 must exist in the **forward**
   chain (not `input`), above any drop rule, and its counter should increment
   when the backend tries.
4. `/ping <device-ip> interface=<lan-iface>` — if forcing the interface works
   but the plain ping does not, the router is choosing the wrong source
   interface: a routing-table/interface-membership problem.

If the router can reach the display but a **Docker container** cannot, note that
containers are NAT'd behind their host: the traffic reaches the router with the
**host's** source address on `192.168.5.x`. Either give the host a route to
`192.168.4.0/24`, or run the backend with `network_mode: host`.
