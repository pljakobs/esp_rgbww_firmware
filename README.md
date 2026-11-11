[![Build Status](https://github.com/pljakobs/esp_rgbww_firmware/actions/workflows/build_firmware.yml/badge.svg?branch=develop)](https://github.com/pljakobs/esp_rgbww_firmware/actions/workflows/build_firmware.yml)

# ESP RGBWW Firmware

Open-source firmware for ESP8266/ESP32-based RGBWW(CW) LED controllers. Depending on the SoC, a controller can use up to 16 hardware PWM outputs across multiple configured lights; each light supports up to 5 color channels (R, G, B, WW, CW). Built on the [Sming](https://github.com/SmingHub/Sming) framework.

This is a fork of [Patrick Jahns' original firmware](https://github.com/patrickjahns/esp_rgbww_firmware) and [VBS's extension](https://github.com/verybadsoldier/esp_rgbww_firmware), significantly extended with multi-controller synchronisation, a rich JSON HTTP API, MQTT with Home Assistant auto-discovery, OTA updates, mDNS, rsyslog support, and a modern Vue/Quasar web application.

---

## Table of Contents

1. [Supported Hardware](#supported-hardware)
2. [Features](#features)
3. [Installation & Flashing](#installation--flashing)
4. [OTA Updates](#ota-updates)
5. [Initial Setup](#initial-setup)
6. [Configuration Reference](#configuration-reference)
7. [HTTP API Reference](#http-api-reference)
8. [Color Command Reference](#color-command-reference)
9. [MQTT Integration](#mqtt-integration)
10. [Multi-Controller Synchronisation](#multi-controller-synchronisation)
11. [Remote Logging (rsyslog)](#remote-logging-rsyslog)
12. [Event Server](#event-server)
13. [WebSocket API](#websocket-api)
14. [Security](#security)
15. [Building from Source](#building-from-source)
16. [Integration: FHEM](#integration-fhem)
17. [Contributing](#contributing)
18. [Firmware Architecture](FIRMWARE_ARCHITECTURE.md)

---

## Supported Hardware

| SoC | Status | PWM frequency |
|-----|--------|---------------|
| ESP8266 | ✅ Primary | 800 Hz |
| ESP32 | ✅ Supported | 4 000 Hz |
| ESP32-C3 | ✅ Supported | configurable |

### QuinLED devices and templates

The firmware bundles a versioned hardware catalog of QuinLED device profiles,
available to clients from `GET /catalog`. Catalog presence does not necessarily
mean that every device is supported for analog LED control in firmware 5.1:

| Board | SoC | 5.1 LED support |
|-------|-----|-----------------|
| QuinLED An-Penta-Mini | ESP32-C3 | Analog PWM |
| QuinLED An-Penta-Deca | ESP32 | Analog PWM |
| QuinLED An-Penta-Plus | ESP32 | Analog PWM |
| QuinLED Dig-Uno v4 | ESP32 | Not yet supported for analog LEDs |
| QuinLED Dig-Quad v4 | ESP32 | Not yet supported for analog LEDs |
| QuinLED Dig-Next-2 | ESP32 | Not yet supported for analog LEDs |

The QuinLED Dig devices have addressable LED data outputs rather than analog
PWM outputs. Their catalog entries are groundwork for the 5.2 addressable-light
extension; they are not currently supported for analog LED control in 5.1.

It also includes these starter templates:

| Template | Board |
|----------|-------|
| Mini RGBWW | QuinLED An-Penta-Mini |
| Deca RGBWW on LED 1-5 | QuinLED An-Penta-Deca |
| Plus RGBWW | QuinLED An-Penta-Plus |

Templates provide frontend setup defaults; selecting one does not implicitly
activate hardware configuration. The frontend resolves board pin IDs to GPIO
numbers before saving the active configuration.

### M3 hardware schema (5.1 feature branch)

The M3 schema replaces `hardware.pinconfigs` with a separation between SoC
capabilities, frontend catalog data and the active device configuration:

- `pins.cfgdb`: SoC GPIO records describing input/output, PWM, strapping,
  internal pull capabilities and reserved resources, stored in a separate ConfigDB.
- `hardware-catalog.json.gz`: frontend-only board inventories and templates,
  kept as a filesystem working copy and streamed by `GET /catalog`.
- `hardware.configuration`: active light types with GPIO-number pin arrays,
  per-light color settings, clear input, buttons and buses.

The frontend resolves template board-pin IDs to physical GPIOs before writing
active configuration. Firmware validates GPIO assignments against the selected
SoC's capability mask; it does not need board maps or templates. Templates never
become active implicitly, and GPIO0 remains a valid physical pin.

The SoC pin seed lives in `soc-pins.json`. Board/template defaults are embedded
as FSTR resources, copied to LittleFS when missing, and upgraded only when the
filesystem catalog revision is older than the firmware seed. A version sidecar
allows that comparison without decompressing or parsing the catalog.

Configured lights are validated by type and numeric GPIO against the SoC
capability ConfigDB, then instantiated as independent `RGBWWLed` devices. A
hardware ConfigDB commit queues a runtime reload; PWM callback drain and
resource validation are checked before running instances are replaced. Button,
clear-input and I2C setup also use the numeric GPIO and electrical properties
from active configuration.

The 5.1 runtime supports per-light commands and readback. HTTP requests select
a light with `?light=N`; JSON-RPC, WebSocket and MQTT commands accept
`"light": N`. A command may also include the light's persistent `light_id` as
an identity check. If no selector is supplied, the unique configured default
light is used. WebSocket clients can subscribe once to the `lights` channel for
multiplexed per-light state notifications and hardware-configuration change
notifications. The 5.1 ESP32 PWM channel planner allocates outputs across
available LEDC speed-mode banks and rejects unavailable or conflicting
assignments.

The HTTP API specification is available at `/api/openapi.json`, with an
interactive Swagger UI at `/api/docs`. Both are served by the firmware; the
Swagger UI assets themselves require an internet connection.

The interim M3 string-ID pin schema was never released and does not need
migration. Any 5.0-to-5.1 migration is a separate compatibility task; the 5.0
schema and runtime are unchanged. The 5.1 development branch remains in
progress, and supported upgrade behavior should be checked for the specific
build before upgrading.

An earlier, unreleased M3 schema stored board labels such as `L1` in light
`pins`, button `pin`, clear-pin and I2C SDA/SCL fields. It is not a released
format and requires no migration. The current active schema stores numeric GPIOs.
5.0’s separate legacy pin configuration is unchanged; any supported 5.0-to-5.1
migration is a separate compatibility task.

Schema fixture tests:

```sh
node tools/generate-api-schemas.mjs
node --test .github/tests/test_hardware_schema.mjs .github/tests/test_virtual_light.mjs .github/tests/test_ledctrl.mjs
```

---

## Features

### Optional Status Display

An I2C SSD1306 128x64 OLED can show the controller name and IP address.
Enable it for the target SoC, for example:

```sh
make -j8 SMING_ARCH=Esp32 SMING_SOC=esp32c3 STATUS_DISPLAY=1
```

Until M2 device templates provide pin configuration, the temporary QuinLED An MINI
defaults are SDA GPIO6 / SCL GPIO7 on ESP32-C3 and SDA GPIO15 / SCL GPIO16 on
ESP32. Override either pin with `STATUS_DISPLAY_SDA` or `STATUS_DISPLAY_SCL`.
Other SoCs require explicit values for both pins. These defaults will be replaced
by device-template configuration later.

Check your board's PWM, button, boot-strapping and other pin assignments before
using these GPIOs on other hardware. Connect the display to 3.3V and GND, with I2C pull-ups to 3.3V.
The default 7-bit address is `0x3c`; override it with
`STATUS_DISPLAY_ADDRESS=0x3d` if needed. No reset GPIO is required.

The firmware probes the address at boot before constructing `StatusDisplay`,
which derives from `Adafruit_SSD1306`. If no device acknowledges, no display
object is allocated and no refresh timer is started. An acknowledgement detects
presence, not the device model; the configured address must belong to the OLED.
Connecting a display after boot requires a restart.

The screen updates within one second of name or network changes, and is only
redrawn when values change. Whenever the local AP is enabled, an `AP ON` screen
takes priority, even if the station is also connected. It shows the AP IP,
active SSID and password in plain text, or `(open network)` for an unsecured AP.
The SSID wraps over two lines and passwords over up to four lines, accommodating
a full 64-character password. Anyone with physical access can read these credentials.
When the AP is disabled, the screen shows the controller name and station IP,
or `Not connected`. Names use the built-in ASCII font and occupy up to three
21-character lines; longer names are truncated.
Display support is disabled by default and is not available in Host builds.

### Core
- Up to 16 hardware PWM outputs across configured lights, depending on SoC; each light supports up to 5 output channels (RGB, RGBWW, RGBCW, RGBWWCW)
- Smooth, programmable on-board fades and animations
- Independent per-channel animation queues with configurable queue policies
- Relative color commands (+/- values on any channel)
- Command requeuing enabling seamless animation loops
- Pause / continue / stop / skip animation queue controls
- Instant blink command
- Toggle (on ↔ off)
- Ramp speed and ramp time specification
- Startup color: `LAST` (restore previous) or `SET` (fixed color)
- Hardware push-button support with configurable debounce

### Network & Communication
- HTTP JSON API on port 80
- WebSocket push events (color changes, transitions, notifications)
- MQTT publish/subscribe with optional Home Assistant auto-discovery
- TCP event server for state-change broadcasts
- mDNS/Bonjour device advertisement
- NTP time synchronization
- UDP rsyslog logging to any syslog collector

### Updates & Management
- OTA firmware updates from configurable update URL
- Dual-ROM partition layout — safe A/B rollback on ESP8266/ESP32
- LittleFS for persistent config storage
- Captive portal for initial Wi-Fi setup
- Basic HTTP authentication for API access

### Color & Calibration
- HSV color model (hue 0–359°, saturation 0–100, value 0–100)
- HSVCT: HSV extended with colour temperature
- RAW 8-bit channel output (r, g, b, ww, cw)
- Per-channel brightness scaling (0–100%)
- Warm-white / cold-white Kelvin calibration (default 2700 K / 6000 K)
- Gamma correction
- 12-segment parabolic HSV calibration (per 30° hue sector)
- Parabolic colour-temperature curve calibration
- Multiple output modes (RGB, RGBWW, RGBCW, RGBWWCW)

---

## Installation & Flashing

Pre-compiled binaries are published as GitHub Actions artifacts on every successful build.

### Initial flash (serial)

```bash
esptool.py --port /dev/ttyUSB0 write_flash 0x00000 firmware.bin
```

Refer to the [Wiki — Flashing](https://github.com/verybadsoldier/esp_rgbww_firmware/wiki/1.1-Flashing) for platform-specific instructions and address maps.

### Flash layout (ESP8266, 4 MB)

```
0x000000  Partition table
0x002000  rom0  (active firmware)
0x100000  lfs0  (~1 000 kB, active LittleFS)
0x202000  rom1  (OTA target)
0x300000  lfs1  (~1 000 kB, fallback LittleFS)
```

---

## OTA Updates

Firmware updates are managed through the webapp's **Firmware Update** dialog
(*Settings → Firmware Update → Check firmware*). The dialog fetches a firmware
manifest from the configured update server and filters available builds by:

- **Release line** — `5.0` or `5.1`
- **Stage** — `Develop`, `Test`, or `Stable`, corresponding to `develop`,
  `testing`, and `prod`
- **Build type** — `release` or `debug`
- **Version** — a specific build, with its date shown in the list

The release line and stage together identify the firmware branch: for example,
`5.0/develop` or `5.1/testing`. Choose a line supported by the controller's
SoC: 5.0 supports ESP8266, ESP32, and ESP32-C3; 5.1 supports ESP32 and ESP32-C3.
In particular, 5.1 builds do not support ESP8266.

The dialog shows the currently installed firmware and webapp versions for
comparison. Selecting a build and clicking **Update** starts the OTA process,
with live progress reported over WebSocket.

The update server URL defaults to `https://lightinator.de/version.json` and is configurable via the `/config` API key `ota.url`.

> **Important:** An active OTA process blocks `/config`, `/color`, and other write endpoints; the API returns `{"error":"update in progress"}` (HTTP 400) during this time.

The update URL serves a JSON array of firmware entries. Each entry identifies
its release line and stage, build type, firmware version, date, supported SoC,
and binary download URL.

> **Note:** Command-line / scripted OTA is not yet documented. A TUI update tool or integration into the Lightinator Log Service companion is planned.

---

## Initial Setup

### Network initialisation flow

On every boot the firmware runs the following sequence:

```
Boot
 └─ No SSID stored?
      ├─ Yes → first-run mode
      │         ├─ Start AP:  SSID = "Lightinator_<chipid>",  IP = 192.168.4.1
      │         ├─ Start DNS captive portal (redirects all DNS → 192.168.4.1)
      │         └─ Scan for available networks (results cached for /networks)
      │
      └─ No  → station mode
                ├─ Static IP configured? → apply IP/mask/gateway, disable DHCP
                └─ DHCP (default)        → enable DHCP
                Connect → retry up to DEFAULT_CONNECTION_RETRIES times
                     ├─ Connected
                     │    ├─ Set hostname (device_name, or "Lightinator_<chipid>")
                     │    ├─ Start network services (HTTP, WebSocket, event server)
                     │    ├─ Get IP → start mDNS, register controller, start MQTT (if enabled)
                     │    ├─ Report any saved crash dump via syslog
                     │    └─ Stop AP after short delay (1 s normal / 90 s new-connection)
                     │
                     └─ Failed / wrong password
                          ├─ Start AP (fallback)
                          └─ Scan for networks
```

### First-time setup steps

1. **Power on** — with no credentials stored the controller starts an access point:
   - SSID: `Lightinator_<chipid>` (e.g. `Lightinator_12AB34`)
   - Password: `rgbwwctrl`
   - IP: `192.168.4.1`
2. **Connect** a phone or laptop to the AP network.
3. **Open the web UI** at `http://192.168.4.1/` — a DNS captive portal redirects all traffic there automatically.
4. **Enter Wi-Fi credentials** (SSID / password) in *Settings → Network* and save. The controller scans for available networks on boot, so they are already listed in the dropdown.
5. The controller connects to your network. The AP stays up for **90 seconds** to allow the UI to follow the redirect before shutting down.
6. Once connected the device is reachable at:
   - **IP address** shown in the station-connected notification (or via your router's DHCP table)
   - **`<device_name>.local`** via mDNS (once a device name is configured)
7. **Set the pin configuration** for your hardware in *Settings → Hardware* (`general.current_pin_config_name`). A reboot is required for pin changes to take effect.

### Connection failure behaviour

If the controller cannot reach the saved network (wrong password, out of range, etc.) after the configured number of retries, it:
- Falls back to its own AP so you can always reach it to reconfigure
- Continues scanning for the target network in the background
- Reports the error via the event server / WebSocket (`connection_status` notification)

---

## Configuration Reference

Configuration is read and written via `GET /config` and `POST /config` using a flat JSON document.
Changes take effect immediately for most settings; settings noted below require a reboot.

### `general`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `device_name` | string | `""` | Hostname used for mDNS and MQTT client ID |
| `current_pin_config_name` | string | `"mrpj"` | Active hardware pin map name (**reboot required**) |
| `pin_config_url` | string | remote URL | URL for the pin configuration manifest |
| `supported_color_models` | array | `["RGB","RGBWW","RGBCW","RGBWWCW"]` | Models shown in UI |
| `buttons_debounce_ms` | integer | 50 | Hardware button debounce time |
| `allow_web_icons` | boolean | true | Allow the webapp to load remote icons |

### `color`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `color_mode` | integer | 0 | Output mode: 0=RGB, 1=RGBWW, 2=RGBCW, 3=RGBWWCW (**reboot required**) |
| `startup.mode` | string | `"LAST"` | Startup colour: `LAST` or `SET` |
| `startup.color` | string | — | Colour string when `startup.mode` is `SET` |
| `brightness.red/green/blue/ww/cw` | integer | 100 | Per-channel brightness correction (0–100) |
| `colortemp.ww` | integer | 2700 | Warm-white colour temperature in Kelvin |
| `colortemp.cw` | integer | 6000 | Cold-white colour temperature in Kelvin |
| `calibration.gamma` | number | — | Output gamma (e.g. 2.2) |
| `calibration.hsv` | array[12] | — | Parabolic coefficients per 30° hue sector |
| `calibration.ct` | object | — | Parabolic coefficients for CT curve |
| `hsv.red/green/blue/…` | integer | 0 | HSV hue shift per colour sector |

### `network.mqtt`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | boolean | false | Enable MQTT client |
| `server` | string | `"mqtt.local"` | Broker hostname or IP |
| `port` | integer | 1883 | Broker TCP port |
| `username` | string | `""` | Broker username |
| `password` | string | `""` | Broker password |
| `topic_base` | string | `"home/"` | Base topic prefix |
| `homeassistant.enable` | boolean | true | Publish HA discovery messages |
| `homeassistant.discovery_prefix` | string | `"homeassistant"` | HA discovery prefix |
| `homeassistant.node_id` | string | `""` | HA node ID (defaults to device name) |

### `network.connection`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `dhcp` | boolean | true | Use DHCP (disable for static IP, **reboot required**) |
| `ip` | string | `"192.168.1.1"` | Static IP (**reboot required**) |
| `netmask` | string | `"255.255.255.0"` | Static netmask |
| `gateway` | string | `"192.168.1.255"` | Static gateway |

### `network.ap`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `ssid` | string | `"Lightinator_<chipid>"` | Access-point SSID |
| `password` | string | `"configesp"` | Access-point password |
| `secured` | boolean | true | Require AP password |

### `network.rsyslog`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | boolean | false | Send logs via UDP syslog |
| `host` | string | — | Collector hostname or IP |
| `port` | integer | 514 | Collector UDP port |

### `events`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `server_enabled` | boolean | true | Enable TCP event server |
| `color_interval_ms` | integer | 500 | Minimum ms between colour events |
| `color_min_interval_ms` | integer | 500 | Hard minimum interval |
| `trans_fin_interval_ms` | integer | 1000 | Interval for transition-finished events |

### `sync`

See [Multi-Controller Synchronisation](#multi-controller-synchronisation).

---

## HTTP API Reference

All endpoints are on port **80**. Responses are JSON. When security is enabled, HTTP Basic Auth
(`Authorization: Basic <base64>`) is required on every request.

**Global response codes**

| HTTP | Body | Meaning |
|------|------|---------|
| 200 | `{"success":true}` | Command accepted |
| 400 | `{"error":"<msg>"}` | Bad request / missing param |
| 401 | — | Unauthorised (security enabled) |
| 429 | — | Insufficient heap; retry after 4 s |
| 503 | — | OTA in progress (ESP8266 only) |

CORS headers are always present. `OPTIONS` requests are answered with `200 {"success":true}`.

Many of the read and command endpoints below are also available over the WebSocket JSON-RPC API described in [WebSocket API](#websocket-api). HTTP remains fully supported; the webapp now prefers WebSocket first and falls back to HTTP when required.

---

### `GET /info`

Returns device identity and runtime state.

Also available via WebSocket JSON-RPC methods `info` and `getInfo`.

**Query parameters:** `?v=2` selects the v2 format (recommended). V2 is sparse by
default: it returns relatively stable device, filesystem, connection and
configuration state. Add `&sparse=0` to include volatile runtime and debug
counters as well. The uppercase aliases `V` and `S` are also accepted.

**Full v2 response excerpt (`/info?v=2&sparse=0`):**

```json
{
  "version": 2,
  "device": {
    "deviceid": 3221339823,
    "soc": "esp8266",
    "current_rom": "rom0"
  },
  "app": {
    "git_version": "abc1234",
    "build_type": "release",
    "git_date": "2026-04-05",
    "webapp_version": "1.2.3"
  },
  "sming": { "version": "5.1.0" },
  "filesystem": {
    "total_bytes": 1000000,
    "free_bytes": 750000,
    "used_bytes": 250000
  },
  "runtime": {
    "uptime": 3600,
    "heap_free": 24576,
    "minfreeHeapRuntime": 20000,
    "minfreeHeap10min": 21000,
    "heapLowErrUptime": 0,
    "heapLowErr10min": 0
  },
  "debug": {
    "http_active_connections": 1,
    "websocket_connections": 1,
    "eventserver_clients": 0
  },
  "rgbww": { "version": "2.0.0", "queuesize": 10 },
  "connection": {
    "connected": true,
    "ssid": "MyNetwork",
    "dhcp": true,
    "ip": "192.168.1.50",
    "netmask": "255.255.255.0",
    "gateway": "192.168.1.1",
    "mac": "AA:BB:CC:DD:EE:FF"
  },
  "mqtt": {
    "enabled": true,
    "status": "running",
    "broker": "mqtt.local",
    "topic": "home/"
  }
}
```

---

### `GET /color`

Returns the current output state.

Also available via WebSocket JSON-RPC methods `color` and `getColor`.

```json
{
  "raw": { "r": 255, "g": 128, "b": 0, "ww": 0, "cw": 0 },
  "hsv": { "h": 1.047, "s": 1.0, "v": 1.0, "ct": 0 }
}
```

> Note: HSV values in the GET response are in **radians** (h: 0–2π, s/v: 0–1). POST commands use degrees (0–359) and 0–100 percent.

### `POST /color`

Send a color command. See [Color Command Reference](#color-command-reference).

---

### `GET /config`

Returns the full configuration as JSON. Suitable for backup and programmatic inspection.

### `POST /config`

Writes a partial or full configuration update. Only provided keys are changed.

```bash
curl -X POST http://<device>/config \
  -H "Content-Type: application/json" \
  -d '{"network":{"rsyslog":{"enabled":true,"host":"192.168.1.10","port":5514}}}'
```

**Side effects on write:**

| Setting changed | Effect |
|-----------------|--------|
| Static IP | Delayed reboot (3 s) |
| SSID | Delayed reboot (3 s) |
| DHCP → static | Delayed reboot |
| DHCP → DHCP | Applied immediately |
| MQTT enabled/disabled | Started/stopped immediately |
| Color mode | Delayed reboot (1 s) |
| Pin config name | Delayed reboot (1 s) |
| Device name | Applied immediately, mDNS updated |
| rsyslog host/port | Applied immediately |
| rsyslog enabled | Applied immediately |
| Telemetry | Started/stopped immediately |

---

### `GET /networks`

Returns available Wi-Fi networks (up to 25). Trigger a scan first with `POST /scan_networks`.

Also available via WebSocket JSON-RPC methods `networks` and `getNetworks`.

```json
{
  "scanning": false,
  "available": [
    { "id": 12345, "ssid": "MyNetwork", "signal": -62, "encryption": "WPA2" }
  ]
}
```

### `POST /scan_networks`

Initiates an asynchronous Wi-Fi scan. Poll `GET /networks` for results.

Also available via WebSocket JSON-RPC command `scan_networks`.

---

### `POST /connect`

Submit Wi-Fi credentials to connect to a network.

```json
{ "ssid": "MyNetwork", "password": "s3cr3t" }
```

---

### `GET /ping`

Simple availability check. Returns `{"ping":"pong"}`.

---

### `POST /system`

System control commands.

Also available via WebSocket JSON-RPC command `system`.

| `cmd` | Description |
|-------|-------------|
| `restart` | Reboot the device |
| `factory` | Reset to factory defaults |
| `debug` | Enable/disable debug output (also requires `"enable": true/false`) |

```bash
curl -X POST http://<device>/system -d '{"cmd":"restart"}'
```

---

### `POST /on` / `POST /off`

Turn the output on (restore last colour) or off immediately.

Also available via WebSocket JSON-RPC commands `on` / `off` (plus aliases `setOn` / `setOff`).

### `POST /toggle`

Toggle between on and off.

Also available via WebSocket JSON-RPC command `toggle`.

---

### `POST /stop`

Stop animation and clear queue. Equivalent to skip + clear.

Also available via WebSocket JSON-RPC command `stop`.

### `POST /skip`

Skip the current animation step, jumping immediately to its end state.

Also available via WebSocket JSON-RPC command `skip`.

### `POST /pause`

Pause the animation queue.

Also available via WebSocket JSON-RPC command `pause`.

### `POST /continue`

Resume a paused animation queue.

Also available via WebSocket JSON-RPC command `continue`.

### `POST /blink`

Trigger an instant blink of the current color.

Also available via WebSocket JSON-RPC command `blink`.

```json
{ "ramp": 500 }
```

---

### `GET /data` / `POST /data`

Read and write the application data store (groups, scenes, presets, controller registry).

### `GET /hosts`

Returns known peer controllers registered in the data store.

---

### `GET /update` / `POST /update`

`GET /update` returns the current firmware OTA status as `{"status":<integer>}`.
`POST /update` starts a firmware update from the supplied ROM URL:

```json
{ "rom": { "url": "https://example.invalid/firmware.bin" } }
```

Webapp updates use separate endpoints. `POST /webapp_check` requests an update
check; `GET /webapp_check` and `GET /webapp_status` return the current webapp
update status.

---

## Color Command Reference

The `/color` endpoint (POST) and the MQTT color topic accept the same JSON command format.

### Single command — HSV fade

```json
{
  "cmd": "fade",
  "hsv": { "h": 120, "s": 100, "v": 80 },
  "ramp": 2000,
  "direction": 0,
  "queue": "single",
  "requeue": false,
  "name": "my-fade"
}
```

### Single command — solid RAW

```json
{
  "cmd": "solid",
  "raw": { "r": 255, "g": 0, "b": 0, "ww": 128, "cw": 0 },
  "ramp": 0
}
```

### Multi-command (batch)

```json
{
  "cmds": [
    { "cmd": "fade", "hsv": { "h": 0,   "s": 100, "v": 100 }, "ramp": 1000 },
    { "cmd": "fade", "hsv": { "h": 120, "s": 100, "v": 100 }, "ramp": 1000, "requeue": true }
  ]
}
```

### From/To fade

Animate from an explicit start colour to a target:

```json
{
  "cmd": "fade",
  "hsv": { "h": 240, "s": 100, "v": 80 },
  "hsvFrom": { "h": 0, "s": 100, "v": 80 },
  "ramp": 3000
}
```

### Relative values

Any numeric field can be specified as a relative string (delta):

```json
{ "cmd": "solid", "hsv": { "h": "+30", "s": "0", "v": "-10" } }
```

### Per-channel targeting

Address specific PWM channels (0=R, 1=G, 2=B, 3=WW, 4=CW) without affecting others:

```json
{ "cmd": "fade", "hsv": { "v": 50 }, "channels": [3, 4], "ramp": 1000 }
```

### Field reference

| Field | Type | Description |
|-------|------|-------------|
| `cmd` | string | `"solid"` (default) or `"fade"` |
| `hsv` | object | HSV target: `h` (0–359), `s` (0–100), `v` (0–100), `ct` (0–10000) |
| `raw` | object | RAW target: `r`, `g`, `b`, `ww`, `cw` (0–255) |
| `hsvFrom` | object | Explicit HSV start (optional) |
| `rawFrom` | object | Explicit RAW start (optional) |
| `ramp` | number | Transition time in ms, or speed value |
| `direction` | integer | Hue rotation direction: 0=forward, 1=reverse |
| `queue` | string | Queue policy: `"single"`, `"back"`, `"front"`, `"replace"` |
| `requeue` | boolean | Re-insert command at queue end after completion (looping) |
| `name` | string | Command name (reported in transition-finished events) |
| `channels` | array | Target specific channel indices only |

---

## MQTT Integration

MQTT is disabled by default. Enable via the configuration.

### Published topics

| Topic | Payload | Retain | Description |
|-------|---------|--------|-------------|
| `<base>color` | `{"raw":{...},"cmd":"solid","t":0}` | yes | Current output (raw) |
| `<base>color` | `{"hsv":{...},"cmd":"solid","t":0}` | yes | Current output (HSV) |
| `last/will` | `"The connection from this device is lost :("` | yes | LWT |

Where `<base>` is the configured `topic_base` (default: `home/`).

### Subscribed topics (slave mode)

| Config key | Purpose |
|------------|---------|
| `sync.color_slave_topic` | Receive color commands |
| `sync.cmd_slave_topic` | Receive JSON-RPC commands |
| `sync.clock_slave_topic` | Receive master clock ticks |

### Home Assistant auto-discovery

When `network.mqtt.homeassistant.enable` is `true`, the firmware publishes MQTT discovery messages for:

- A main `light` entity (full RGBWW control)
- Individual channel `light` entities

Discovery prefix defaults to `homeassistant`. Node ID defaults to the device name.

---

## Multi-Controller Synchronisation

Multiple controllers can be synchronised to play identical animations in lock-step. One controller acts as **master**, the others as **slaves**.

### Clock synchronisation

The master publishes a tick counter at `sync.clock_master_interval` ms intervals. Slaves receive ticks and dynamically adjust their PWM timer interval using a PLL-style algorithm to eliminate accumulated drift.

### Command relay

When `sync.cmd_master_enabled` is `true`, every color/animation command received by the master is re-published to the command slave topic. Slaves with `sync.cmd_slave_enabled` execute these commands, achieving synchronised playback.

### Colour relay

An alternative mode (`sync.color_master_enabled`) publishes the master's current output values at `sync.color_master_interval_ms` for less-precise but simpler following.

### Sync configuration keys

| Key | Default | Description |
|-----|---------|-------------|
| `sync.clock_master_enabled` | false | Publish master clock ticks |
| `sync.clock_master_interval` | 30 ms | Clock publish interval |
| `sync.clock_slave_enabled` | false | Follow master clock |
| `sync.clock_slave_topic` | `"home/led1/clock"` | Clock MQTT topic |
| `sync.cmd_master_enabled` | false | Relay commands to slaves |
| `sync.cmd_slave_enabled` | false | Execute relayed commands |
| `sync.cmd_slave_topic` | `"home/led/command"` | Command relay MQTT topic |
| `sync.color_master_enabled` | false | Publish colour state |
| `sync.color_master_interval_ms` | 0 | Colour publish interval |
| `sync.color_slave_enabled` | false | Follow master colour |
| `sync.color_slave_topic` | `"home/led1/command"` | Colour MQTT topic |

---

## Remote Logging (rsyslog)

The firmware can forward log messages to any UDP syslog collector (RFC 3164 / RFC 5424) in real time.

Enable via `/config`:

```json
{
  "network": {
    "rsyslog": {
      "enabled": true,
      "host": "192.168.1.10",
      "port": 5514
    }
  }
}
```

Changes take effect immediately without a reboot.

Runtime-info health sampling continues every ten seconds, but its WebSocket
notification is only constructed when an open connection subscribes to
`runtime_info`. The notification reuses that tick's heap measurements rather
than probing the allocator again.

### Callback timing diagnostics

Callback timing instrumentation is opt-in: build debug firmware with
`make ... EVENT_MONITOR=1`. The default `EVENT_MONITOR=0` compiles out scopes,
timers, registry storage and CallbackTiming/DispatcherTiming/DispatcherTrace
reporting. Release builds also compile these out even when the flag is enabled.
The existing EventQueueMonitor queue-health probe is independent of this flag.
Sming's debug dispatcher counters are controlled separately by its debug build;
this firmware flag controls their collection/reporting, not the framework hooks.

Debug builds keep callback timing counters in RAM and send `CallbackTiming`
summaries directly to the configured syslog stream every five seconds,
independently of the normal debug verbosity filter. Release builds omit the
accounting and reporting.

`runtime_info` also has three nested measurements: `runtime_collect` for heap
sampling/health accounting, `runtime_json` for ConfigDB population and JSON
serialization, and `runtime_enqueue` for the synchronous WebSocket send/enqueue
call (not eventual network delivery). Collection runs without subscribers;
JSON/enqueue only run with runtime-info subscribers. The outer callback's
heap-status and render-success log lines remain outside these subsections, so
their cost appears in `runtime_info` but not the subsection totals. Logging
inside the WebSocket send call remains included in `runtime_enqueue`.

Each reporting window lists active paths in descending average elapsed time:

```text
CallbackTiming seq=1 window_ms=5000 inclusive=1 order=avg_us
CallbackTiming seq=1 name=led_update calls=250 avg_us=180.0 max_us=900 total_us=45000
```

New diagnostic builds emit versioned runtime-derived paths:

```text
CallbackTiming seq=1 window_ms=5000 inclusive=1 order=avg_us format=2 dropped_scopes=0
CallbackTiming seq=1 format=2 name=led_update/light_state/render calls=20 avg_us=800.0 max_us=1100 total_us=16000
```

`name` is the full synchronous measurement path, with `/` separating components.
Its parent is the prefix before the last `/`; a single-component name is a root
scope. Sibling paths are separate scoped operations; ancestor totals include
descendants. These are instrumented scopes, not a complete function call graph.
The same function invoked in another context has a separate path, e.g.
`led_update/light_state/send/ws_pump`, `current_state/send/ws_pump` or a deferred
top-level `ws_pump`. Dispatcher batches are not synthetic parents: TCP/IP work
can execute outside a dispatcher batch. Async work does not inherit the scope
that queued it. All durations still include blocking and preemption.

Both state publications now have `root` (ConfigDB root construction and store
open), `populate` (update acquisition, state conversion/setters and updater
destruction), `render` (JSON-RPC export), `normalize` (RAW union normalization,
including its HSV no-op path) and `send` (synchronous logging, copying and
TCP/WebSocket send/enqueue) children. Surrounding work and root destruction stay
in the parent, so subsection totals do not exhaust the parent's duration.

Timing uses a fixed 96-path registry with at most eight levels, no per-call heap
allocation, and static report snapshots. Unrepresentable scopes and their
descendants are omitted rather than attributed to an incorrect parent;
`dropped_scopes` reports their entry count per reporting interval. Counters
are recorded when scopes finish: child and parent completions can belong to
different windows, so subtracting child totals in a single window does not
produce reliable exclusive time. The statistics tool reads both formats,
deriving format-2 parents from paths and retaining hints for legacy flat names.

`calls`, `avg_us`, `max_us` and `total_us` describe completed calls since the
previous report, not lifetime statistics. Timings include blocking waits and
preemption, so a high value means time spent occupying the callback, not
necessarily CPU computation. Nested timings are inclusive: for example,
`led_update` includes `light_show`, state publication and any WebSocket pumping
it invokes. Do not sum those totals as independent CPU usage.

Instrumented paths cover LED updates, individual light updates, actual
light/current-state publications (excluding suppressed events), WebSocket
pumping, deferred light reloads, OTA verification and hashing, runtime-info and
telemetry timers, button polling and MQTT message handling. `timing_report`
measures the reporter itself and appears in the following window.

Counters and the reporting timer run only in Sming callback context; there is
no per-call allocation or logging. Reports may be delayed when that thread is
busy, and cannot be emitted while it is completely stalled. `window_ms` shows
the actual reporting interval. Enable rsyslog as above to receive the reports.

For exercise-only operation without rebooting or validating results:

```sh
python3 tests/virtual_light_exerciser.py 192.168.29.224 --permissive
```

`--permissive` uses HTTP regardless of `--via`, skips WebSocket checks and
log-service collection, and reports command/read errors without stopping later
steps or lights. It also works with `--soak-hours`, continuing later cycles after
command failures. Discovery of configured lights is still required; invalid
arguments, discovery failures and user interruption are not suppressed.
Completion returns zero even if exercise errors occurred, with an explicit
error count (not a validation success). Original states are restored when readable
unless `--no-restore` is set; soak mode attempts to turn every light off at exit.

Aggregate retained reports with the standard-library-only statistics tool
(Python 3.10 or later):

```sh
python3 tests/log_statistics.py --host lightinator.local --log 192.168.29.5
python3 tests/log_statistics.py --host lightinator.local --log 192.168.29.5 --callback-timing -b -1 --sort total
python3 tests/log_statistics.py --host lightinator.local --log 192.168.29.5 --dispatcher-timing -b 0
python3 tests/log_statistics.py --host 192.168.29.69 --host 192.168.29.224 --log 192.168.29.5 -b 0 --timeline calls/s
```

`--host` accepts a controller hostname or its syslog source IP; use an explicit
IP if the hostname resolves to multiple addresses. `--log` accepts a log-service
hostname/address with an optional port (default `4821`) or HTTP(S) scheme.
The tool does not contact or reboot the controller.
Repeat `--host` to collect and print separate reports sequentially in argument
order. Boot offsets are resolved independently for each controller; statistics
are never pooled across controllers. A collection failure stops processing and
does not write a partial comparison plot.

Use `--callback-timing` to select the CallbackTiming statistics module explicitly.
Use `--dispatcher-timing` for dispatcher batch durations (weighted average,
maximum and total in ms), processed events, sampled queue peak, rejected event
posts, and mailbox post attempts/rejections. Nonzero rejections also list the
boot and sequence of each affected report window. These counters reset per
report; duplicate boot/sequence reports are counted once. Missing sequences and
UDP loss mean totals can be incomplete. Dispatcher statistics are text-only;
`--sort` applies to the callback table, and plots require CallbackTiming.
Select both flags to include both reports.
With no module flags, all registered modules are reported. `-b 0` (or `--boot 0`)
selects the current/latest retained boot, `-b -1` the previous retained boot, and
`-b -2` two retained boots ago. Without `-b`, all retained boots are included.
Boot selection uses `/api/v1/boots`, independently of whether a boot has timing
records. Unavailable offsets fail explicitly. Positive offsets are not supported.
When pagination is needed, queries start at the selected boot's first retained
log ID and stop at its last ID; records from other boots are excluded even if
late UDP packets interleave their IDs.

Timing columns are displayed in milliseconds. `--sort` accepts `function`,
`calls`, `average` (default), `max`, `total`, `calls/s`, `ms/s` or `included-in`. Text columns sort
alphabetically; numeric columns sort descending, with function names breaking ties.
Every measurement also shows `Calls/s` and `ms/s`: total calls and inclusive
milliseconds divided by the summed observed reporting-window time, including
blocks where that measurement is inactive. Missing sequence blocks are excluded;
missing headers use the reported five-second estimate. Lost measurement records
can understate rates. `ms/s` includes blocking/preemption and is not CPU usage;
parent and child rates must not be added.
Reports and plots warn about logged windows exceeding 60 seconds, which may
indicate firmware clock issues and distort rates; logged durations are preserved.
The table's `Included in` column derives format-2 parents from runtime paths,
even if the parent has no retained measurements. Legacy flat `ws_pump` records
list `synchronous caller` because their nesting is unspecified. `-` means no fixed parent is listed,
not that the measurement is independent or exclusive. Unknown function names
remain visible. `--sort included-in` groups rows by their parent without altering
their inclusive measurements.

Use `--sort tree` for an indented runtime measurement tree: parents appear before
their descendants, and leaf labels are indented by path depth. The `Included in`
column retains the full parent path, so identical leaf names under different
parents remain distinct. Numeric sorts continue to display full measurement paths.
Format-2 headers also identify unqualified names as root scopes, even if individual
records omit `format=2` or UDP delivers the header after its records.

Add `--timeline` for an interactive standalone HTML load plot, or choose `--timeline load`,
`--timeline total`, `--timeline calls`, `--timeline calls/s`, `--timeline ms/s`
or `--timeline all`. Per-second plots normalize each block by its own duration.
Load is inclusive
elapsed callback time divided by the actual reporting-window duration, expressed
as a percentage; the other metrics are total milliseconds or calls per block.
The output defaults to `log-statistics.html`; use `--plot FILE.html` to change
the path (`--plot` alone enables all metrics). Open the printed file URL in a
browser. No packages, CDN, network access or Python GUI environment are needed
to display the plot. Keep `tests/log_statistics_plot.html` beside the Python script.
The HTML includes boot/metric selectors and function checkboxes, with independent
vertical scales and hierarchy labels per function. Hover for precise values;
the module selector labels each controller. Enable **Compare controllers / boots**
to overlay the same measurement from every retained boot of the selected module,
with colored series, legends, shared measurement scales and per-series hover values.
Comparison uses collector `receivedAt` timestamps with a shared zero at the earliest
observed report across all selected controllers, preserving their time offsets.
Device uptime is not used for the horizontal axis. Use `-b 0` to compare only the latest retained
boot of each controller; without a boot filter, all retained boots are overlaid.
Drag to zoom the time axis across all panels and reset with the button or
double-click. Missing function records break the curves rather than becoming
zero; orange bands mark missing sequences. The horizontal axis uses actual
collector receive intervals, not device-reported window durations. First reports
and reports after sequence gaps cover an estimated preceding five seconds, clipped
at zero; gaps retain their actual collector-time spacing. Missing receive timestamps
fail explicitly rather than mixing timebases. The t=0 timestamp is printed in the
page. Rate/load calculations still use device-reported window durations (or an
explicit five-second estimate for missing headers). Inclusive load is not independent CPU
utilization and functions are not stacked. The summary table remains in the
terminal; unreadable ASCII timelines are no longer emitted.

The report includes the timing hierarchy; no child totals are subtracted and
no additive "overall load" is calculated:

| Measurement | Included measurements/work |
|-------------|----------------------------|
| DispatcherTiming batch duration | Synchronous instrumented and uninstrumented batch work, plus dispatcher overhead |
| `led_update` | All `light_show` calls; `light_state` and `current_state` when publication occurs |
| `runtime_info` | `runtime_collect`, `runtime_json`, `runtime_enqueue`, surrounding logging/overhead |
| `ota_verify` | `ota_hash` |
| `timing_report` | Diagnostic formatting and syslog output, including frozen trace output |
| `ws_pump` | Stream pumping; its initial synchronous invocation also belongs to its caller |
| `light_state`, `current_state` | `root`, `populate`, `render`, `normalize`, `send`; any synchronous instrumented send callees |
| `light_show`, runtime subsections, `ota_hash` | No explicitly nested timing buckets in current normal paths |
| `light_reload`, `telemetry`, `buttons`, `mqtt_receive` | Whole operation, including any instrumented functions invoked synchronously |

`light_state` and `current_state` are siblings of `light_show`, not included in
it. Deferred callbacks belong to their later dispatcher batch, not the callback
that queued them. MQTT reception runs in TCP/IP context and need not belong to a
Sming dispatcher batch. Durations include blocking and preemption; never sum
parent and child totals. DispatcherTiming is context here, not a plotted metric
of the CallbackTiming module.

The tool first queries `/api/v1/search` for each registered statistic. This API
is capped at 200 global matches without pagination or IP filtering; if the cap
is reached, the tool announces a fallback to paginated `/api/v1/logs` for the
controller. It reports per-function calls, call-weighted average, maximum and
total milliseconds. Sequence numbers identify five-second blocks separately
per boot; the summary reports observed blocks and internal sequence gaps, using
actual `window_ms` values where available and estimating missing headers at
five seconds. Only received records contribute, so UDP loss or log retention
can leave statistics incomplete. Inclusive function totals are not independent
CPU utilization measurements.

To add another statistic, define `cli_option`, `search_term`, `consume(record)`,
`report(sort, timeline)` and `plot_data()` (matching the existing plot data shape,
or returning `None` for text-only modules)
and register its class in `STATISTICS` in
[`tests/log_statistics.py`](tests/log_statistics.py).

On ESP32, the locally instrumented Sming dispatcher also emits
`DispatcherTiming`: completed batch count, processed events, average/maximum/
total batch elapsed microseconds, maximum events per batch, event-queue depth
sampled at batch entry, rejected event posts, and attempted/rejected Sming
wake-up posts to the TCP/IP mailbox. `mbox_full` counts actual `ERR_MEM`
rejections; it is not a mailbox occupancy sample or a count of network packet
drops. The mailbox itself is private to lwIP and is not modified or probed.
`queue_peak_sampled` is a lower bound on peak queue depth, not a continuous
high-water mark. Counters reset per report; the batch containing the report
finishes afterward and is included in the following window.

The dispatcher also retains the last 32 completed callbacks and the currently
executing callback in a fixed-size RAM flight recorder. The first mailbox
`ERR_MEM` freezes the completed history and captures the active callback, failure
timestamp and sampled Sming queue depth. Further rejections are counted without
overwriting that incident. The next five-second report takes and rearms the
recorder, emitting `DispatcherTrace` lines oldest first. Callbacks completed
while frozen are not retained; `overwritten` counts older completed entries
replaced before freezing. This is not a TCP/IP mailbox snapshot or recovery trace.

Each entry includes a batch number, boot-relative `start_us`, wall-clock
`elapsed_us`, and raw hexadecimal `sig`/`par`. For `state=active`, elapsed time
ends at failure, not callback completion. Decode addresses with the exact flashed
build's ELF, for example `riscv32-esp-elf-addr2line -a -f -C -e
out/Esp32/esp32c3/debug/build/app.out 0xADDRESS` (use the corresponding Xtensa
toolchain for other ESP32 variants). `sig` is the dispatched function address;
`par` may be a function address for a parameterless callback, but is often a
data/delegate pointer. Shared timer/delegate wrappers may not identify the
underlying application callback. A trace correlates activity with rejection,
not proof that a particular callback caused mailbox saturation.

Dispatcher accounting includes two clock reads per completed callback while
recording, batch timing, and short critical sections protecting shared diagnostic
state. Freezing allocates nothing and neither logs nor enqueues work. Reporting
up to 33 entries adds syslog cost to the reporting callback itself. All dispatcher
diagnostics are omitted from release builds. Firmware using these diagnostics
needs the matching local Sming `sming_dispatch_stats_take` and
`sming_dispatch_trace_take` APIs.

Run `python3 tests/test_dispatch_trace.py` for native dispatcher and report-format
regressions; it compiles the local Sming source with test doubles and also checks
release/non-network builds. Set `SMING_HOME` if Sming is not at `/opt/sming/Sming`.

### Lightinator Log Service

A companion container service ([lightinator-log-service](https://github.com/pljakobs/lightinator-log-service)) provides:

- UDP syslog ingestion (port 5514 by default)
- HTTP API for log retrieval and live tail
- Integration with the webapp Log Viewer

**Setup flow:**
1. Run the collector container on a local host.
2. In the webapp Log Viewer, enter the collector's IPv4 and HTTP port.
3. The webapp configures the controller's rsyslog target automatically.
4. Logs appear live in the web interface.

---

## Event Server

When `events.server_enabled` is `true`, the controller broadcasts state-change notifications over a persistent TCP connection. Compatible with FHEM and custom integrations.

Event types pushed:

- `color` — current colour value (at most every `color_interval_ms`)
- `transition_finished` — emitted after an animation command completes (includes `name` and `requeued` flag)
- `notification` — human-readable messages (config changes, reboots, etc.)

---

## WebSocket API

Connect to `ws://<device>/ws`.

The socket serves two roles:

- **JSON-RPC request/response API** for reads and commands
- **push events** for live state updates and notifications

### JSON-RPC request format

```json
{ "jsonrpc": "2.0", "id": 1, "method": "info", "params": { "V": "2" } }
```

### JSON-RPC success response

```json
{ "jsonrpc": "2.0", "id": 1, "result": { "device": { "soc": "esp8266" } } }
```

### JSON-RPC error response

```json
{ "jsonrpc": "2.0", "id": 1, "error": "method not implemented" }
```

### Supported request/command methods

| Method | Purpose |
|--------|---------|
| `info`, `getInfo` | Read runtime/device information |
| `color`, `getColor` | Read current color state |
| `config`, `getConfig` | Read full configuration |
| `networks`, `getNetworks` | Read current Wi-Fi scan results |
| `scan_networks` | Trigger asynchronous Wi-Fi scan |
| `system` | Execute system command (`restart`, `debug`, etc.) |
| `color` | Execute color / animation payload |
| `on`, `setOn` | Turn output on |
| `off`, `setOff` | Turn output off |
| `toggle` | Toggle on/off |
| `stop` | Stop animation and clear queue |
| `skip` | Skip current transition |
| `pause` | Pause animation queue |
| `continue` | Resume paused queue |
| `blink` | Trigger blink |
| `direct` | Apply direct color change |
| `subscribe` with `channel: "runtime_info"` | Subscribe this connection to runtime-info updates |
| `unsubscribe` with `channel: "runtime_info"` | Stop runtime-info updates |
| `runtime_info_subscribe`, `subscribe_runtime_info` | Legacy runtime-info subscribe aliases |
| `runtime_info_unsubscribe`, `unsubscribe_runtime_info` | Legacy runtime-info unsubscribe aliases |
| `webapp_check` | Request a webapp update check |
| `authenticate` | Complete the challenge-response auth handshake (see [Security](#security)) |

The `keep_alive` message is reserved for connection maintenance and is handled automatically by the webapp.

When `security.api_secured` is enabled, every method except `authenticate` and
`keep_alive` requires the connection to first complete the challenge-response
handshake described in [Security](#security). Unauthenticated calls are rejected
with JSON-RPC error `-32001` and a fresh `challenge`.

### Push events

The controller also pushes the same events as the TCP event server:

```json
{ "type": "color",   "data": { "raw": { "r": 255 }, "hsv": { "h": 1.047 } } }
{ "type": "transition_finished", "data": { "name": "my-fade", "requeued": false } }
{ "type": "notification", "data": "new SSID, MyNetwork" }
```

After a connection subscribes to the `runtime_info` channel, it also receives
`runtime_info` notifications containing current runtime counters. The
subscription is per WebSocket connection.

The webapp uses this connection both for live status updates and for request/response API traffic when available.

---

## Security

API access can be protected by a single shared password. It is **disabled by
default**. Enable it and set the password via the configuration API:

```json
{ "security": { "api_secured": true, "api_password": "mysecret" } }
```

There is **no username** — the password is the only shared secret. It is used
by both the HTTP REST API (HTTP Basic) and the WebSocket JSON-RPC API
(challenge-response). When `api_secured` is `false`, all endpoints are open and
no credentials are checked.

### HTTP REST API — HTTP Basic

When enabled, every HTTP request must include an `Authorization` header:

```
Authorization: Basic <base64(":" + password)>
```

The username portion is ignored, so the base64 payload is simply a colon
followed by the password (e.g. `base64(":mysecret")`). A request without valid
credentials is answered with:

```
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Basic realm="RGBWW Server"
Connection: close
```

### WebSocket JSON-RPC API — challenge-response

The WebSocket API does **not** use HTTP Basic. Instead each connection must
complete a per-connection challenge-response handshake before any state-changing
method is accepted. The `authenticate` method and `keep_alive` pings are always
allowed through; every other method requires an authenticated connection.

**1. Trigger a challenge.** Send any secured method (or an `authenticate`
request with no hash). If the connection is not yet authenticated, the
controller replies with JSON-RPC error code `-32001` and a top-level
`challenge` — a 32-character lowercase-hex nonce (16 random bytes):

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": { "code": -32001, "message": "authentication required" },
  "challenge": "3f9a1c...e7"
}
```

**2. Compute the response hash.** Concatenate the challenge and the password
with a colon separator and take the lowercase-hex SHA-256 digest:

```
hash = SHA256( challenge + ":" + password )   // lowercase hex
```

**3. Send the `authenticate` request** with the computed hash:

```json
{ "jsonrpc": "2.0", "id": 2, "method": "authenticate", "params": { "hash": "<hash>" } }
```

**4. Result.** On success the controller marks the connection authenticated and
consumes the nonce:

```json
{ "jsonrpc": "2.0", "id": 2, "result": { "authenticated": true } }
```

On failure it returns error `-32001` again together with a **fresh** `challenge`
(the previous nonce is single-use), so the client can retry. Because the
challenge is per-connection and single-use, the hash cannot be replayed on a new
connection.

> Browsers compute the digest with `crypto.subtle.digest("SHA-256", ...)`; the
> firmware uses the same `SHA256(challenge + ":" + password)` formula, so both
> sides must produce identical lowercase-hex output.

The AP password defaults to `configesp` and should be changed before deployment.


---

## Building from Source

### CI build container

The container workflow and Dockerfile remain in this repository. Pushes to
`pljakobs/Sming`'s `develop` branch dispatch
`.github/workflows/build-container.yml` on `5.1/develop`, passing the exact
Sming commit SHA. The image retains the `pjakobs/sming:latest`, `develop`, and
version tags; its OCI revision label records the source SHA.

The fork-only trigger requires an Actions secret named
`SMING_CONTAINER_DISPATCH_TOKEN` in `pljakobs/Sming`, containing a token with
Actions write permission on `pljakobs/esp_rgbww_firmware`. DockerHub credentials
stay in this repository. Manual container builds may omit `sming_commit` to use
current Sming `develop`; the daily update check remains enabled.

### Prerequisites

- [Sming framework](https://github.com/SmingHub/Sming) installed and `SMING_HOME` set
- `make`, `esptool`, and the appropriate GCC toolchain for your target SoC

### Build

```bash
# ESP8266
make SMING_ARCH=Esp8266

# ESP32
make SMING_ARCH=Esp32

# Host (unit tests / simulation)
make SMING_ARCH=Host
```

### Flash

```bash
make flash SMING_ARCH=Esp8266 COM_PORT=/dev/ttyUSB0
```

The `flash` and `deployOta` scripts in the repository also handle OTA deployment to running devices.

---

## Integration: FHEM

A ready-made FHEM device module is available:
[https://github.com/verybadsoldier/esp_rgbww_fhemmodule](https://github.com/verybadsoldier/esp_rgbww_fhemmodule)

See also the [FHEM Forum thread](https://forum.fhem.de/index.php?topic=70738.0) for community support.

---

## Contributing

Pull requests are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting.

---

## Links

- [Sming Framework](https://github.com/SmingHub/Sming)
- [RGBWWLed Library](https://github.com/verybadsoldier/RGBWWLed)
- [Webapp (esp_rgb_webapp2)](https://github.com/pljakobs/esp_rgb_webapp2)
- [Lightinator Log Service](https://github.com/pljakobs/lightinator-log-service)
- [FHEM Module](https://github.com/verybadsoldier/esp_rgbww_fhemmodule)
- [FHEM Forum](https://forum.fhem.de/index.php?topic=70738.0)
