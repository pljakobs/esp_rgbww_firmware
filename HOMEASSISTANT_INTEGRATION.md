# Home Assistant Integration Plan

## Overview

The firmware already has partial MQTT-based HA auto-discovery, but with critical bugs that prevent it from working correctly. The plan is structured in three phases:

- **Phase 1** — Fix critical MQTT bugs in `app/mqtt.cpp` (already done, see fix branches)
- **Phase 2** — MQTT quality improvements in `app/mqtt.cpp`
- **Phase 3** — Native HA custom component via HTTP REST + WebSocket

---

## Phase 1 — Fix Critical MQTT Bugs (Firmware)

> Status: **done** — see branches `fix/mqtt-availability-topic`, `fix/mqtt-color-temp-command`, `fix/mqtt-state-color-mode`, `fix/mqtt-rediscover-on-reconnect`

### 1.1 — LWT / Availability Topic

**Problem:** LWT was sent to a hardcoded `"last/will"` topic with a freeform message. HA had no way to mark the device unavailable on disconnect.

**Fix:**
- Change LWT topic from `"last/will"` to `buildTopic("availability")` (per-device)
- Change LWT payload from freeform message to `"offline"`
- Publish `"online"` to the availability topic on every successful connect
- Add `availability_topic`, `payload_available`, `payload_not_available` to the HA discovery config payload

### 1.2 — Handle `color_temp` Commands

**Problem:** `handleHomeAssistantCommand()` only read `doc["color"]` (HS format). When HA sent a `color_temp` command (mireds), it was silently ignored.

**Fix:**
- Add `else if (doc.containsKey("color_temp"))` branch
- Convert mireds → firmware CT scale (0–100): `ct = (mireds - 153) * 100 / 217`
  - 153 mireds (~6500 K, cool white) → ct = 0
  - 370 mireds (~2700 K, warm white) → ct = 100
- Set saturation to 0 (pure white) and apply brightness alongside the CT value

### 1.3 — Correct `color_mode` in State Messages

**Problem:** `publishHAState()` always emitted `"color_mode": "hs"` regardless of whether the warm/cool-white channels were driving the output. This violates the HA MQTT JSON light contract.

**Fix:**
- Check `getCurrentColorMode()` to determine whether the device supports colour temperature (RGBWW or RGBWWCW)
- When CT > 0 on a CT-capable device: emit `color_mode: "color_temp"` and `color_temp: <mireds>`
- Otherwise: emit `color_mode: "hs"` with the `color: {h, s}` object

### 1.4 — Re-publish Discovery Config on Reconnect

**Problem:** `_haConfigPublished` was set to `true` after the first publish and never cleared. If the MQTT broker restarted and lost retained messages, the firmware would not re-send discovery payloads.

**Fix:**
- Reset `_haConfigPublished = false` at the top of `onConnected()` so discovery is re-published on every reconnection

---

## Phase 2 — MQTT Quality Improvements (Firmware)

### 2.1 — Fix `object_id` Hardcoding

- `_haObjectId` is currently hardcoded to `"1"`, resulting in HA entity IDs like `light.mylight_1`
- Change to use the device name (hostname from ConfigDB, same source as `_haNodeId`)

**Feasibility:** ✅ straightforward — device name already available via `general.getDeviceName()`

**⚠ Breaking change:** This changes the MQTT topic path from `.../1/set` to `.../mydevice/set`. Any existing HA installation will end up with orphaned stale entities for the old path and new duplicate entities for the new path. Must be done as a coordinated release alongside 2.4 (which also changes topic paths), with a clear migration note in the changelog.

### 2.2 — Add `configuration_url`

- Add `"configuration_url": "http://<ip>"` to the device info block in the discovery config
- `WifiStation.getIP().toString()` is available (same call used in webserver.cpp)

**Feasibility:** ✅ easy, purely additive — no breaking change, no extra memory pressure.

### 2.3 — Increase Discovery Config Buffer

- `StaticJsonDocument` is **stack-allocated**, not heap — so this is stack pressure, not heap fragmentation. Heap is not directly impacted.
- Phase 1 already raised the main discovery config doc from 512 → 768 bytes to fit the availability fields
- At 768 bytes ArduinoJson v6 can hold roughly 400–500 bytes of serialised JSON before overflow; the current discovery payload with availability fields is close to this limit on devices with long names
- On ESP32 (typical 8 KB task stack) 768 is fine; on ESP8266 (smaller stack) raising further is risky

**Feasibility:** ✅ with caveats — rather than blindly increasing the size, add a `doc.overflowed()` check after serialisation and log a warning. This detects silent truncation without wasting stack. A further increase to 1024 is acceptable on ESP32 builds only (guard with `#ifdef ARCH_ESP32`).

### 2.4 — Fix Per-Channel Entity Discovery Payloads

- Per-channel configs currently emit `"color_mode": false`, which is not a valid field in the HA MQTT JSON light schema
- The intended design: the Lightinator supports **HSV mode** (all 3–5 channels driven together as a colour) and **RAW mode** (each channel controlled independently as a 0–1023 brightness value). Per-channel entities represent RAW control.
- Correct fix: replace `"color_mode": false` with `"supported_color_modes": ["brightness"]` — this correctly describes a brightness-only light in HA's schema.

**Feasibility:** ✅ correct and safe. `["brightness"]` exactly matches the RAW per-channel intent.

**⚠ Breaking change:** If 2.1 also changes the `object_id` (and thus the channel topic paths), both 2.1 and 2.4 must land in the same release to avoid partial migration states in HA.

---

### Phase 2 Summary

| Item | Feasibility | Risk | Priority |
|---|---|---|---|
| 2.1 `object_id` | ✅ easy | ⚠ Breaking — coordinate with 2.4 | Medium |
| 2.2 `configuration_url` | ✅ trivial | None | High (quick win) |
| 2.3 Buffer size | ✅ with overflow check | Low (stack, not heap) | Medium |
| 2.4 Channel payloads | ✅ easy | ⚠ Breaking — coordinate with 2.1 | Medium |

---

## Phase 3 — Native HA Custom Component (Python)

A native component gives full access to the device's capabilities (presets, scenes, animations, multi-controller groups) without requiring an MQTT broker.

### Location

New repository or standalone folder: `custom_components/esp_rgbww/`

### Architecture

```
HA Component
  ├── ConfigFlow     — mDNS discovery (_esprgbwwAPI._http._tcp) + manual IP
  ├── HTTP REST      — commands via POST /color, /on, /off, /toggle, /stop, /skip
  ├── WebSocket /ws  — real-time state push (primary)
  └── Entities
        ├── light.{name}            main RGBWW light (HS + color_temp + brightness)
        ├── light.{name}_ww         warm-white channel (brightness only)
        ├── light.{name}_cw         cool-white channel (brightness only)
        ├── button.{name}_stop      stop animation queue
        ├── button.{name}_skip      skip to end of current animation step
        ├── button.{name}_pause     pause queue
        ├── button.{name}_continue  resume paused queue
        ├── select.{name}_preset    load named presets from /data
        └── sensor.{name}_*         heap, uptime, RSSI from /info?v=2
```

### Files

| File | Purpose |
|---|---|
| `manifest.json` | name, domain, version, dependencies: `aiohttp`, `zeroconf` |
| `__init__.py` | `async_setup_entry`, `async_unload_entry`, shared aiohttp session |
| `const.py` | domain, default port (80), scan interval (30 s), mDNS service type |
| `strings.json` | UI labels for config flow |
| `config_flow.py` | mDNS discovery + manual IP fallback; validates via `GET /info?v=2` |
| `coordinator.py` | `DataUpdateCoordinator`; WebSocket primary, polling fallback |
| `light.py` | `LightEntity` — `ColorMode.HS` + `ColorMode.COLOR_TEMP` |
| `button.py` | stop / skip / pause / continue |
| `sensor.py` | heap, uptime, RSSI |
| `select.py` | preset list from `GET /data` |

### Step 3.1 — Scaffold

- `manifest.json` with `aiohttp` + `zeroconf` dependencies
- `__init__.py`: `async_setup_entry` creates one aiohttp `ClientSession` stored in `hass.data`
- `const.py`: `DOMAIN = "esp_rgbww"`, `DEFAULT_PORT = 80`, `MDNS_TYPE = "_esprgbwwAPI._http._tcp.local"`
- `strings.json`: labels for config flow steps

### Step 3.2 — Config Flow

- **Discovery**: `ZeroconfServiceInfo` for `_esprgbwwAPI._http._tcp.local`; auto-populated host + name
- **Manual**: IP address text field
- **Validation**: `GET /info?v=2` — confirm response, extract `chip_id`, `name`, firmware version
- **Config entry data**: `host`, `port`, `name`, `chip_id`

### Step 3.3 — Data Coordinator

- Subclass `DataUpdateCoordinator`
- On startup: connect WebSocket to `ws://{host}/ws`
- WS message `type: "color"`: parse `data.hsv` and `data.raw`, notify all listeners
- WS message `type: "transition_finished"`: fire `hass.bus.async_fire("esp_rgbww_transition_finished", ...)`
- WS disconnect: fall back to `GET /color` polling every 30 s; retry WS reconnect with backoff
- Coordinator data shape: `{"hsv": {h, s, v, ct}, "raw": {r, g, b, ww, cw}, "info": {...}}`

### Step 3.4 — Light Entity

- `ColorMode.HS` and `ColorMode.COLOR_TEMP`
- `color_temp_range`: min 153 mireds, max 370 mireds
- `turn_on` mapping:

  | HA parameter | Firmware field | Conversion |
  |---|---|---|
  | `brightness` (0–255) | `v` (0–100) | `v = brightness * 100 / 255` |
  | `hs_color` (0–360, 0–100) | `h`, `s` | direct |
  | `color_temp` (mireds) | `ct` (0–100) | `ct = (mireds - 153) * 100 / 217` |
  | `transition` (seconds) | `ramp` (ms) | `ramp = transition * 1000` |

- `turn_off`: POST `/off`
- `effect_list`: preset names from `GET /data` → `presets[].name`
- `turn_on(effect=...)`: POST `/color` with the matching preset's HSV

### Step 3.5 — Button Entities

| Entity | Endpoint |
|---|---|
| `button.{name}_stop` | POST `/stop` |
| `button.{name}_skip` | POST `/skip` |
| `button.{name}_pause` | POST `/pause` |
| `button.{name}_continue` | POST `/continue` |

### Step 3.6 — Sensor Entities

| Entity | Source | Unit |
|---|---|---|
| `sensor.{name}_heap` | `/info?v=2` → `heap` | bytes |
| `sensor.{name}_uptime` | `/info?v=2` → `uptime` | s |
| `sensor.{name}_rssi` | `/info?v=2` → `rssi` | dBm |

### Step 3.7 — Select Entity (Presets)

- Options: `GET /data` → `presets[].name`, refreshed on each coordinator update
- On selection: POST `/color` with the preset's HSV values
- Current selection: match coordinator state against preset colors (best-effort)

---

## API Reference (Firmware HTTP Endpoints Used)

| Method | Endpoint | Used by |
|---|---|---|
| `GET` | `/info?v=2` | Config flow validation, sensor entities |
| `GET` | `/color` | Coordinator polling fallback |
| `GET` | `/data` | Preset list for light effects + select entity |
| `POST` | `/color` | Light turn_on, preset select |
| `POST` | `/on` | Light turn_on (state only) |
| `POST` | `/off` | Light turn_off |
| `POST` | `/stop` | Stop button |
| `POST` | `/skip` | Skip button |
| `POST` | `/pause` | Pause button |
| `POST` | `/continue` | Continue button |
| `WS` | `/ws` | Coordinator real-time push |

mDNS service type: `_esprgbwwAPI._http._tcp.local` (already advertised by firmware)

---

## Verification

### Phase 1 + 2 (Firmware MQTT)

1. Flash firmware, configure MQTT broker in device settings
2. HA MQTT integration → "Listen to topic" `homeassistant/light/#` — verify config payloads contain `availability_topic`, correct `object_id`, `configuration_url`
3. Subscribe to availability topic — verify `"online"` on connect, `"offline"` on power-off/disconnect
4. Send `color_temp` command from HA UI — verify light changes colour temperature and state reflects `color_mode: "color_temp"`
5. Restart MQTT broker — verify discovery payloads are re-published on reconnect

### Phase 3 (Custom Component)

1. Install component in `custom_components/`, restart HA
2. mDNS auto-discovery: device appears in HA integrations dashboard without manual configuration
3. Manual setup: enter IP address, verify config flow completes and all entities appear
4. Turn on/off from HA UI — device responds in < 200 ms
5. Set HS color, colour temperature, brightness — all three colour modes work independently
6. Set effect (preset name) — preset colour is applied on the device
7. Simulate network drop — WebSocket disconnects, fallback polling maintains current state
8. Stop / skip / pause / continue buttons — animation queue responds correctly

---

## Open Decisions

1. **Component location** — Separate GitHub repository vs. subfolder of the firmware repo vs. the webapp2 repo
   -> separate repo
2. **Authentication** — Device supports optional HTTP Basic password; include in config flow from the start or add later?
   -> currently out of scope
3. **Phase ordering** — MQTT fixes (Phase 1+2) can be developed independently of the custom component (Phase 3)

4. **What UI will homeassistant use?

## Out of Scope

- Multi-controller scene/group management in HA (future phase)
- FHEM module changes
- Lightinator log service changes
