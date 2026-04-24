[![Build Status](https://github.com/pljakobs/esp_rgbww_firmware/actions/workflows/build_firmware.yml/badge.svg?branch=develop)](https://github.com/pljakobs/esp_rgbww_firmware/actions/workflows/build_firmware.yml)

# ESP RGBWW Firmware

Open-source firmware for ESP8266/ESP32-based RGBWW(CW) LED controllers, supporting up to 5 independent PWM channels (R, G, B, WW, CW). Built on the [Sming](https://github.com/SmingHub/Sming) framework.

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

---

## Supported Hardware

| SoC | Status | PWM frequency |
|-----|--------|---------------|
| ESP8266 | ✅ Primary | 800 Hz |
| ESP32 | ✅ Supported | 4 000 Hz |
| ESP32-C3 | ✅ Supported | configurable |

### Known pin configurations

| Name | SoC | R | G | B | WW | CW | Clear |
|------|-----|---|---|---|----|----|-------|
| mrpj | ESP8266 | 13 | 12 | 14 | 5 | 4 | 16 |
| Lightinator Mini | ESP32-C3 | 0 | 4 | 5 | 6 | 7 | 18 |
| NodeMCU ESP32-C3 01M | ESP32-C3 | 5 | 7 | 6 | 9 | 10 | 21 |
| Shojo PCB (WemosD1 mini) | ESP8266 | 14 | 4 | 5 | 15 | 12 | 16 |

Pin configurations are fetched at runtime from a remote JSON file (default:
`https://raw.githubusercontent.com/pljakobs/esp_rgb_webapp2/devel/public/config/pinconfig.json`)
and can be overridden in the configuration.

---

## Features

### Core
- Up to 5 independent PWM output channels (RGB, RGBWW, RGBCW, RGBWWCW)
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

Firmware updates are managed through the webapp's **Firmware Update** dialog (*Settings → Firmware Update → Check firmware*). The dialog fetches a firmware manifest from the configured update server and presents three cascading selectors:

- **Branch** — `Stable`, `Testing`, or `Develop` (colour-coded: green/amber/red)
- **Build type** — `release` or `debug`
- **Version** — specific build, with date shown in the dropdown

The dialog shows the currently running firmware version, branch, and webapp version for comparison. Selecting a firmware entry and clicking **Update** triggers the OTA process with live progress reporting via WebSocket.

The update server URL defaults to `https://lightinator.de/version.json` and is configurable via the `/config` API key `ota.url`.

> **Important:** An active OTA process blocks `/config`, `/color`, and other write endpoints; the API returns `{"error":"update in progress"}` (HTTP 400) during this time.

The manifest format served at the update URL is a JSON array of firmware entries, each containing `branch`, `type`, `fw_version`, `date`, and a direct download URL for the binary.

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

**Query parameter:** `?v=2` — returns the extended v2 format (recommended).

**v2 response:**

```json
{
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
  "runtime": {
    "uptime": 3600,
    "heap_free": 24576,
    "minimumfreeHeapRuntime": 20000,
    "minimumfreeHeap10min": 21000,
    "heapLowErrUptime": 0,
    "heapLowErr10min": 0
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

Also available via WebSocket JSON-RPC methods `config` and `getConfig`.

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

### `GET /update`

Triggers a firmware OTA check and update from the configured update URL.

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
| `hosts`, `getHosts` | Read known peer controllers |
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

The `keep_alive` message is reserved for connection maintenance and is handled automatically by the webapp.

### Push events

The controller also pushes the same events as the TCP event server:

```json
{ "type": "color",   "data": { "raw": { "r": 255 }, "hsv": { "h": 1.047 } } }
{ "type": "transition_finished", "data": { "name": "my-fade", "requeued": false } }
{ "type": "notification", "data": "new SSID, MyNetwork" }
```

The webapp uses this connection both for live status updates and for request/response API traffic when available.

---

## Security

Disabled by default. Enable via:

```json
{ "security": { "api_secured": true, "api_password": "mysecret" } }
```

When enabled, every HTTP request must include `Authorization: Basic <base64(:<password>)>`. There is no username — use just the password preceded by a colon in the base64-encoded string.

The AP password defaults to `configesp` and should be changed before deployment.

---

## Building from Source

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
