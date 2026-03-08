# Plan: Multi-Light LedCtrl Support

**TL;DR:** Replace the single `APPLedCtrl rgbwwctrl` with a `vector<APPLedCtrl>`, extend the hardware config schema to describe multiple named lights (each with a type and pins), add a `light` parameter to all API calls (defaulting to light 0 for backward compat), and tag EventServer/MQTT payloads with the light name. The `RGBWWLed` submodule needs minor surgery to handle fewer than 5 active pins and a new `LightType` concept. Delivered in 4 incremental phases.

---

## Phase A — Foundation *(blocks everything else)*

### Step 1: `RGBWWLed` submodule — `LightType` + single-channel routing
- Add `LightType` enum (`RGBWW`, `RGBCW`, `RGB`, `WW`, `CW`, `SINGLE`) to `Components/RGBWWLed/RGBWWTypes.h`
- Add `setLightType()` to `RGBWWLed`; modify `setOutput(HSVCT&)` so single-type lights map only `v` (brightness) to the active channel
- `PWMOutput` / `Esp32HardwarePwm`: accept -1 for unused pins and skip LEDC channel allocation for them — `Esp32HardwarePwm` already uses `vector<uint8_t>& pins`, so only active pins get passed

### Step 2: `LightConfig` struct + schema in firmware
- Add to `include/ledctrl.h`:
  ```cpp
  enum class LightType { RGBWW, RGBCW, RGB, WW, CW, SINGLE };
  struct LightConfig { String name; LightType type; PinConfig pins; };
  ```
- Extend `app-config.cfgdb` hardware section: pinconfig entries get a `lights` array (items: `name`, `type`, `channels[{name,pin}]`). Backward compat: if `lights` absent, treat legacy flat `channels` as one light named "main" of type rgbww
- Extend `app-data.cfgdb`: `last-color` (single HSVCT) → `last-colors` object keyed by light name

### Step 3: `APPLedCtrl` multi-instance init *(depends on step 2)*
- Add `int _lightIndex` and `String _lightName` to `include/ledctrl.h`
- Change `APPLedCtrl::init()` in `app/ledctrl.cpp` to `init(int lightIndex, const LightConfig& cfg)`
- ESP32: compute `channelStart` = sum of channels of all prior lights (e.g., RGBWW light 0 uses channels 0–4, so light 1 starts at channel 5)
- `colorSave()`/`colorReset()` read/write `last-colors[_lightName]`

### Step 4: `Application` singleton → vector *(depends on step 3)*
- `include/application.h`: change `APPLedCtrl rgbwwctrl` → `std::vector<std::unique_ptr<APPLedCtrl>> lights`; add:
  ```cpp
  APPLedCtrl* getLight(int idx = 0);
  APPLedCtrl* getLight(const String& name);
  ```
- Keep a deprecated `APPLedCtrl& rgbwwctrl = *lights[0]` alias to limit the diff in later phases
- `app/application.cpp`: `init()` iterates configured lights, calls `lights[i]->init(i, cfg)`

### Step 5: Shared 50 Hz timer *(parallel with step 4)*
- Move `_ledTimer` from `APPLedCtrl` to `Application`; single callback iterates all lights, calling `updateLed()` on each — keeps all strips frame-synchronized

---

## Phase B — API Routing *(depends on Phase A)*

### Step 6: `JsonProcessor` light resolution
- Add `APPLedCtrl* resolveLight(JsonObject& root)` in `app/jsonprocessor.cpp`:
  - Resolves `root["light"]` (integer index or string name)
  - Missing/null → returns `lights[0]`
- Replace all `app.rgbwwctrl.XXX()` calls with `resolveLight(root)->XXX()`
- `serializeState()` includes `"light": name`

### Step 7: Webserver pass-through *(parallel with step 6)*
- All endpoints in `app/webserver.cpp` read optional `light` query param or body field and inject it before dispatching to jsonproc
- `GET /info` returns an array of all lights with name, type, and current state

### Step 7b: `create_light` management API *(depends on steps 2–4)*
- New JSON-RPC method `create_light` in `app/jsonprocessor.cpp`:
  ```json
  { "method": "create_light", "params": { "name": "accent", "type": "SINGLE", "channels": [{"name": "coldwhite", "pin": 18}] } }
  ```
- New HTTP endpoint `POST /light` in `app/webserver.cpp` that forwards to `jsonproc.onCreateLight()`
- `JsonProcessor::onCreateLight(JsonObject root, String& msg)` must:
  1. Validate `name` (non-empty, no duplicates, sanitize to alphanumeric + hyphen)
  2. Validate `type` is a known `LightType` string
  3. Validate each channel pin: must be valid for the target SoC and not already claimed by another light
  4. Validate total channel count won't exceed PWM ceiling for the SoC (ESP32/ESP8266: 8 total)
  5. Append a new `lights` entry to the active pinconfig in `AppConfig::Hardware` and persist via ConfigDB
  6. Instantiate a new `APPLedCtrl`, call `init(app.lights.size(), newLightConfig)`, push to `app.lights`
  7. Subscribe to new MQTT topics for the light
  8. Return `{"result": {"index": N, "name": "...", "type": "..."}}` on success
- Companion `DELETE /light/<name>` (or `delete_light` RPC) to remove a light:
  1. Stop the `APPLedCtrl` instance (turns off PWM)
  2. Remove from `app.lights` vector
  3. Remove from ConfigDB hardware config
  4. Delete `last-colors[name]` from AppData
  5. Unsubscribe MQTT topics
  6. **Constraint**: cannot delete the last remaining light (must always have at least one)
  7. Re-index remaining lights so ESP32 `channelStart` values stay contiguous
- Schema file `api-schemas/light.schema.json` to be created describing `create_light` and `delete_light` request bodies

---

## Phase C — Pub/Sub *(parallel with Phase B)*

### Step 8: EventServer multi-light
- `app/eventserver.cpp`: `publishCurrentState()` gains `lightName` param; includes `"light": name` in `color_event` JSON-RPC
- `APPLedCtrl::publishToEventServer()` passes `_lightName`

### Step 9: MQTT multi-light topics
- `app/mqtt.cpp`: all publish/subscribe topics parameterized with light name
- Base topic pattern: `/rgbww/<light_name>/color` (publish), `/rgbww/<light_name>/set` (subscribe)
- Incoming message: parse light name from topic, route to `app.getLight(name)`

---

## Phase D — Persistence *(depends on Step 3)*

### Step 10: Per-light last-color migration
- On first boot after upgrade: copy `last-color` → `last-colors["main"]`, remove old key
- All `colorSave()`/`colorReset()` use per-light keyed storage

---

## Relevant Files

| File | Change |
|---|---|
| `app/ledctrl.cpp` | `init()` refactor, per-light fields |
| `include/ledctrl.h` | `LightConfig`, `LightType`, instance fields |
| `include/application.h` | vector of lights, helper accessors |
| `app/application.cpp` | light instantiation loop, shared timer |
| `app/jsonprocessor.cpp` | `resolveLight()`, `onCreateLight()`, `onDeleteLight()`, all command dispatch |
| `app/webserver.cpp` | `light` param injection, `/info` response, `POST /light`, `DELETE /light/<name>` |
| `app/mqtt.cpp` | parameterized topics |
| `app/eventserver.cpp` | light-tagged state events |
| `app-config.cfgdb` | `lights` array in pinconfigs |
| `api-schemas/light.schema.json` | new — `create_light` / `delete_light` request schema |
| `app-data.cfgdb` | `last-colors` keyed object |
| `Components/RGBWWLed/RGBWWTypes.h` | `LightType` enum |
| `Components/RGBWWLed/RGBWWLed.h/cpp` | `setLightType()`, `setOutput()` routing |
| `Components/RGBWWLed/RGBWWLedOutput.h/cpp` | `-1` pin skipping in `PWMOutput` |

---

## Verification

1. Build ESP32 single-light config → existing behavior unchanged, no API breakage
2. Build ESP32 two-light config (RGBWW + single) → verify no LEDC channel conflicts (max 8 on ESP32)
3. `POST /color {"hsv":{...}}` → targets light 0 (backward compat)
4. `POST /color {"light":"accent","hsv":{...}}` → targets named light independently
5. `GET /info` → returns array with both lights, types, and current states
6. MQTT: `/rgbww/main/color` and `/rgbww/accent/color` fire independently
7. EventServer: `color_event` includes `"light"` field in each message
8. Power cycle: each light restores its own last color
9. `POST /light {"name":"accent","type":"SINGLE","channels":[{"name":"coldwhite","pin":18}]}` → creates light, responds with index
10. `POST /color {"light":"accent","hsv":{...}}` → controls newly created light
11. `DELETE /light/accent` → removes light, subsequent `/info` no longer lists it
12. `POST /light` with a duplicate name → returns 400 error
13. `POST /light` exceeding SoC PWM ceiling → returns 400 error with clear message
14. Existing test suite (`pytest tests/`) passes without regressions

---

## Decisions & Constraints

- **Backward compat**: `app.rgbwwctrl` alias to `lights[0]` reduces initial diff size
- **Single-channel**: uses existing `RGBWWLed` engine with `-1` inactive pins and `LightType::SINGLE`; maps only brightness (HSV `v`) to the one active PWM channel
- **Clock sync**: `StepSync` stays per-instance; one shared timer keeps all lights frame-synced
- **Config migration**: one-time at boot in `Application::init()`
- **ESP32 PWM ceiling**: RGBWW (5ch) + RGB (3ch) = 8ch; validated at init time
- **Target platforms**: ESP32, ESP32-S3, ESP32-C3, ESP32-C2, ESP8266, RP2040
- **API default**: missing `light` → light 0 (fully backward compatible)
- **MQTT topics**: name-based (`/rgbww/<light_name>/...`)

## Further Considerations

1. **ESP8266 PWM ceiling**: Also 8 HW PWM channels via `HardwarePWM`. Needs same `-1` pin skipping in `PWMOutput` ESP8266 path.
2. **RP2040**: 16 PWM slices (32 channels total) — plenty of headroom. Only `isPinValid()` needs updating.
3. **cfgdb migration risk**: Test `last-color` → `last-colors` migration carefully before removing the old key, as ConfigDB schema library behaviour with unknown keys needs verification.
4. **Note**: `app/ledctrl.cpp` already contains a `// ToDo:` comment describing this exact feature, confirming the intent was always planned.
