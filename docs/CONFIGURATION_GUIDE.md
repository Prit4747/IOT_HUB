# Configuration Guide

This is a single ESP-IDF firmware project that can target **three different
cellular modem modules** from the same source tree, picked by a
`menuconfig` choice. Everything not modem-specific — Wi-Fi AP, web UI,
full/delta OTA — is shared and unaffected by which one you pick. This
guide walks a new person through configuring, wiring, building, flashing,
and testing OTA for whichever module they have on the bench.

## 1. What's actually configurable

| Module | Transport | Status |
|---|---|---|
| SimCom A7672S | USB (host) | Working, hardware-validated |
| Quectel EC200U-CN | UART (PPPoS) | Working, hardware-validated |
| Quectel EC200U-CN | USB (host) | Not implemented -- selecting it fails the build on purpose, with a clear message |

The module choice is a **compile-time** decision (`idf.py menuconfig`),
not something the firmware detects at runtime. Switching modules always
means: reconfigure → rebuild → reflash. There's no way to flash one
module's binary and have it "just work" on the other's wiring.

## 2. Prerequisites

- ESP-IDF >= 5.3 set up and activated (`. $HOME/esp/esp-idf/export.sh` or
  your usual method)
- `idf.py set-target esp32s3` already run once in `code/` (only needed
  the very first time this project is built on a machine)
- The physical module wired per section 4 below, **before** you flash --
  a mismatch between what's wired and what's configured produces
  confusing failures, not a clean error

## 3. Manual configuration -- `idf.py menuconfig`

From the `code/` directory:

```sh
idf.py menuconfig
```

1. Navigate to **`IoT Hub Multi-Modem Configuration`**
2. Select **`Modem module`** -- a radio-button list appears with the
   three options from the table above. Pick the one matching your
   hardware.
3. Set **`SIM APN`** to your carrier's APN (e.g. `airtelgprs.com`,
   `jionet`) -- this field is shared across all three modules, no need
   to re-enter it when switching.
4. **Quectel EC200U-CN/UART only**: a submenu **`Quectel EC200U-CN UART
   pins/baud`** appears (only when that module is selected). Confirm or
   adjust:
   - `UART port number` (default 1)
   - `TXD GPIO` / `RXD GPIO` (default 17 / 18)
   - `UART baud rate` (default 115200)
   - `UART flow control` (default None -- this carrier board doesn't
     expose RTS/CTS pins, leave it here regardless of baud)
5. `Esc` repeatedly to back out, `Q` to quit, `Y` to save

Then build:

```sh
idf.py build
```

If you selected a module whose backend isn't implemented yet (Quectel
EC200U-CN/USB), the build fails immediately with a clear message telling
you so -- that's expected, not a bug to chase.

### Doing this without the menuconfig UI

Equivalent, if you'd rather script it or don't have an interactive
terminal: edit `sdkconfig` directly (after at least one `idf.py build` has
generated it) and flip the relevant `CONFIG_MODEM_MODULE_*` lines, then
run `idf.py reconfigure` to regenerate the dependent options (e.g. the
UART pin settings) before building. Not recommended for routine use --
menuconfig is the supported path and won't let you set an invalid
combination.

## 4. Wiring

### SimCom A7672S (USB)

| A7672S | ESP32-S3 |
|---|---|
| USB D+ | Native USB, **GPIO20** |
| USB D- | Native USB, **GPIO19** |
| GND | Common GND |
| VBAT/5V | **Dedicated ≥2A supply** -- not the ESP32's own USB/5V pin |
| PWRKEY | **GPIO4** |

No RST line on this module -- PWRKEY is the only hardware reset path.

**VBUS caveat, board-dependent:**
- Dedicated **ESP32-S3-USB-OTG DevKit**: its `USB_HOST` connector already
  sources VBUS; the firmware also drives GPIO18 (`OTG_USB_SEL_GPIO`)
  automatically to route native USB there. Nothing extra needed.
- Plain **ESP32-S3-DevKitC-style** board: its native USB port has **no
  VBUS-sourcing circuit in host mode**. You must manually wire 5V from the
  board's own 5V pin onto the VBUS line going to the modem, alongside
  D+/D-/GND, or the modem will never enumerate (zero USB activity in the
  log, otherwise-correct wiring notwithstanding).

### Quectel EC200U-CN (UART, Vanix TracX-1b carrier)

| TracX-1b | ESP32-S3 |
|---|---|
| Tx (module transmit) | **GPIO18** (`MODEM_UART_RX_GPIO`) |
| Rx (module receive) | **GPIO17** (`MODEM_UART_TX_GPIO`) |
| GND | Common GND |
| +V_BAT / -V_BAT | **Dedicated ≥2A, 3.7-4.0V supply** |
| Power-select jumper | **`BAT`** position only -- see caution below |
| PWRKEY | **GPIO4** |
| RST | **GPIO5** |

**Power jumper -- do not bridge USB and BAT together.** This carrier
board has a jumper that *selects* one power source or the other (`BAT`
for an external VBAT supply, `USB` for USB-C VBUS power) -- it is never
meant to bridge both at once. Bridging them ties two different voltage
rails together and causes exactly the symptoms of a flaky module:
continuous UART `Rx Break` flooding on the modem link, AT commands
intermittently failing, and the module appearing to reset randomly. If
you see that pattern, check this jumper first.

## 5. Build and first flash

```sh
idf.py build
idf.py -p <PORT> flash monitor
```

Use serial flash (not OTA) for:
- The very first flash of a given module/board combination
- Recovering from firmware that broke the OTA mechanism itself (OTA
  can't fix the thing it depends on to download a fix -- a network-layer
  bug needs a wired flash to get the fix onto the device at all)

Every flash after that, on a device with already-working OTA firmware,
can go over the air instead (section 7).

### Expected log shape

**SimCom (USB):**
```
Installing USB CDC host driver...
usbh_modem installed -- waiting 3000 ms for USB enum / boot
USB CDC device connected, VID: 0x1e0e, PID: 0x9011
Matched USB Modem: "SIMCOM A7600/A7670/A7672"
Modem manufacturer ID: SIMCOM INCORPORATED
Modem auto connect enabled, starting PPP...
PPP GOT_IP  x.x.x.x
PPP: internet OK -- OTA ready
```

**Quectel (UART):**
```
Waiting 5000 ms for modem boot before opening UART...
Initializing esp_modem (generic DCE) for Quectel EC200U-CN on UART1 ...
Modem responding: signal quality rssi=... ber=...
Waiting for network attachment (AT+CGATT) up to 30000 ms...
set_mode(DATA_MODE) -- dialing PPP...
PPP GOT_IP  x.x.x.x
PPP: internet OK -- OTA ready
```

`"internet OK -- OTA ready"` is the line that means the device is fully
online and ready for the web UI / OTA.

## 6. Web UI

1. Join Wi-Fi **`IOT-HUB-AP`** (open, no password)
2. Open `http://192.168.4.1`
3. Status panel shows: active module, firmware version, modem state,
   IP, RSSI

## 7. OTA -- full and delta

Firmware artifacts are kept **per module** (`firmware/<module>/full/` and
`firmware/<module>/delta/`) since a `.bin`/`.patch` built for one module
is never valid for another -- don't cross-apply them.

1. Bump `PROJECT_VER` in `code/CMakeLists.txt`
2. `idf.py build`
3. Host the resulting `.bin` somewhere with a public HTTPS URL. Easiest:
   a GitHub Release on this repo --
   ```sh
   gh release create <tag> build/iot_hub_multi_modem.bin \
     --repo <owner>/<repo> --title "<tag>" --notes "..."
   ```
4. Paste that URL into the web UI's OTA field, click **Download OTA**

### Delta OTA

```sh
python tools/make_patch.py \
  --base firmware/<module>/full/<currently-running>.bin \
  --new  firmware/<module>/full/<new-version>.bin \
  --out  firmware/<module>/delta/<base>_to_<new>.patch \
  --chip esp32s3 --verify
```

The device must be running **exactly** the `--base` binary -- its
SHA256 is checked on-device (`esp_partition_get_sha256()`) against a hash
baked into the patch's header before anything is applied. A mismatch
fails cleanly with `"Patch not for this firmware"`, no partial/corrupt
flash -- just fall back to a full OTA instead.

## 8. Troubleshooting

**`ESP_ERR_HTTP_CONNECT` on every OTA attempt, especially HTTPS, while
plain HTTP or a TCP self-test succeeds** -- almost always a PPP MTU
issue. Cellular PPP paths often silently drop packets above some
carrier-specific size well under the standard 1500-byte MTU; a TLS
handshake's larger packets (ClientHello with SNI/cert-bundle extensions,
multi-KB certificate chain) hit this far more often than a small HTTP
GET does. Already fixed in `ppp_manager.c` (`PPP_MTU`, currently 1300) --
if still flaky, try lowering further.

**Internet self-test passes at boot, then OTA fails minutes later with
every DNS lookup timing out (not instantly rejected)** -- the PPP link
died silently server-side (a common carrier behavior: idle PDP/NAT
sessions get dropped with no explicit terminate sent back), and nothing
detected it. Fixed via LCP echo keepalives
(`CONFIG_LWIP_ENABLE_LCP_ECHO` in `sdkconfig.defaults`) -- a dead link now
gets detected and triggers automatic reconnect within ~15-20s of going
idle-dead instead of never.

**Device needs a manual PWRKEY power-cycle after every ESP reset to
reconnect** -- the ESP32's own USB host peripheral resets whenever the
ESP32 does, which looks like an abrupt disconnect to the modem. Fixed:
`main.c` now runs a PWRKEY power-cycle once, automatically, before the
first `modem_install()` attempt on every boot.

**`"GPIO 17/18 is not usable, maybe used by others"`** (Quectel/UART) --
observed on a second `modem_install()` within the same boot session
(e.g. after an automatic recovery cycle). Has not blocked a connection
in testing so far -- treat as a warning to watch, not yet a confirmed
blocker.

**Quectel module resets erratically / continuous UART `Rx Break` in the
log** -- check the power-select jumper isn't bridging `USB` and `BAT`
(section 4). This produced exactly this symptom in testing.

**A fix to the OTA/network layer itself doesn't seem to take effect over
OTA** -- it can't. The *currently running* firmware is what performs the
OTA download; if that firmware has the bug you're trying to fix, it will
keep failing to download the fix. Serial-flash the fix once (section 5),
then OTA works again from there.
