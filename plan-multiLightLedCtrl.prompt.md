# prerequisite:
- Implement the new light types and shared API behavior on all supported architectures. ESP8266 remains single-light, but its one light may use the new `SINGLE`, `WW`, or `WWCW` profiles; ESP32 supports multiple configured lights.
- Preserve the existing API contract: legacy unindexed calls continue to address the default light with unchanged request/response shapes. The optional light selector and multi-light enumeration are additive; ESP8266 resolves only its single configured light and rejects other selectors.
- The firmware must still build across supported architectures. Keep architecture-specific behavior behind the existing build/source selection model, rather than scattering `#ifdef` conditionals through shared application code.
- ESP8266 heap use is a hard constraint: keep its controller statically embedded, and avoid adding per-light heap allocations, a heap-backed global light vector, or duplicate copies of configuration and JSON data.
- Keep one shared AppConfig/API schema source and use ConfigDB's evaluator-backed `@` attributes to select target-specific references. ESP8266 must not use the multi-light array representation. Enforce target-specific pin and channel limits through the selected hardware provider.
- the underlying RBWWWLED submodule can stay mult arch, but will have to be extended to support one more configuration: WWCW for pure white lights.

### Conditional schema references (proposed)
ConfigDB `dbgen.py` evaluates `@` attributes before resolving `$ref`. Keep the selector in `app-config.cfgdb` and put the target-specific light structures in `refs-Esp8266.cfgdb` and `refs-Esp32.cfgdb`. Select the schema family by `SMING_ARCH`; keep individual SoC pin/channel capabilities in the hardware provider:
```json
"@$ref": "f'refs-{SMING_ARCH}/$defs/light-config'"
```
ConfigDB's cross-file reference form is `<database>/$defs/<path>`; the database names above correspond to the two refs files. Confirm both target builds resolve the expected definition and that the ESP8266 generated model contains no multi-light array. ConfigDB currently discovers root-level `*.cfgdb` files, so verify the unselected refs file does not generate or link unused array support. Ensure API schema projections resolve the same selected reference rather than exposing the raw evaluator expression.

# Plan: Multi-Light LedCtrl Support

**TL;DR:** Add shared light types and API behavior on every architecture. ESP8266 keeps its single embedded `APPLedCtrl`; ESP32 supports multiple lights. Use one AppConfig schema source with evaluator-selected references, preserve legacy API shapes, and add the optional light selector and multi-light discovery additively. Keep ESP8266 heap overhead minimal. The `RGBWWLed` submodule needs type-specific queues and sparse-pin support.

---

## Phase A — Foundation *(blocks everything else)*

### Step 1: `RGBWWLed` submodule — per-type channel profiles
- Add `LightType` enum (`RGBWWCW`, `RGBWW`, `RGBCW`, `RGB`, `WWCW`, `WW`, `CW`, `SINGLE`) to `Components/RGBWWLed/RGBWWTypes.h`
- Supply the type before animation channels are allocated; create only the HSV/raw channel queues required for that type
- For `SINGLE`, route HSV `v` and the raw output through one shared queue
- `PWMOutput` and the architecture-specific PWM backends: accept an unused-pin representation and map the fixed logical RGBWW slots to compact active outputs on every architecture

### Step 2: `LightConfig` struct + schema in firmware
- Add to `include/ledctrl.h`:
  ```cpp
  enum class LightType { RGBWWCW, RGBWW, RGBCW, RGB, WWCW, WW, CW, SINGLE };
  struct LightConfig { String name; LightType type; PinConfig pins; };
  ```
- Proposed light configuration and animation-channel matrix:

  | Type | Physical outputs | HSV queues | Raw queues | Total |
  |---|---|---|---|---:|
  | `RGBWWCW` | R, G, B, WW, CW | H, S, V, CT (4) | R, G, B, WW, CW (5) | 9 |
  | `RGBWW` | R, G, B, WW | H, S, V (3) | R, G, B, WW (4) | 7 |
  | `RGBCW` | R, G, B, CW | H, S, V (3) | R, G, B, CW (4) | 7 |
  | `RGB` | R, G, B | H, S, V (3) | R, G, B (3) | 6 |
  | `WWCW` | WW, CW | V, CT (2) | WW, CW (2) | 4 |
  | `WW` | WW | V/raw share one queue | V/raw share one queue | 1 |
  | `CW` | CW | V/raw share one queue | V/raw share one queue | 1 |
  | `SINGLE` | one configured output | V/raw share one queue | V/raw share one queue | 1 |
- Extend the hardware pinconfig in `app-config.cfgdb` with a `lights` property referencing the selected target structure. `refs-Esp8266.cfgdb` defines one light object; `refs-Esp32.cfgdb` defines an array of light objects, each with `name`, `type`, and `channels[{name,pin}]`. Migrate legacy flat `channels` to one object on ESP8266 or a one-element array on ESP32.
- Extend `app-data.cfgdb`: `last-color` (single HSVCT) → `last-colors` object keyed by light name

### Step 3: `APPLedCtrl` multi-instance init *(depends on step 2)*
- Add `int _lightIndex` and `String _lightName` to `include/ledctrl.h`
- Change `APPLedCtrl::init()` in `app/ledctrl.cpp` to `init(int lightIndex, const LightConfig& cfg)`
- ESP32 baseline: use two 8-channel banks on classic ESP32 (low-speed and high-speed); SoCs without both modes use the banks/capacity reported by the hardware provider. Each logical light must fit wholly within one bank; do not split a light across the channel-8 boundary. Reject configurations that cannot be packed.
- Query per-bank capacity from `ESP32_PWM_MAX_CHANNELS` / `SOC_LEDC_CHANNEL_NUM`; use `ESP32_PWM_TOTAL_CHANNELS` only to determine whether a second speed-mode bank exists, not as one flat channel range. Validate unique/valid GPIOs and each bank-local channel range.
- Configure one LEDC timer per bank, shared by that bank's channels. The bank owns the timer lifetime; per-light PWM views must not independently deconfigure a shared timer. A bank has one frequency/resolution in this baseline.
- Treat light topology, pin order, and bank/channel assignments as boot-time configuration. Changes are persisted and take effect after reboot; do not reallocate or renumber live outputs. Configure one timer per bank at startup and keep each bank stable for the running instance.
- Verify the Pentadeca layout against bank packing: three 5-channel lights cannot fit as whole lights into two 8-channel banks despite 15 aggregate outputs, so that configuration is rejected by the baseline.
- `colorSave()`/`colorReset()` read/write `last-colors[_lightName]`

### Step 4: `Application` light access abstraction *(depends on step 3)*
- Expose common `getLight(index)` / `getLight(name)` accessors so API and service code do not depend on architecture-specific storage.
- ESP8266 resolves light 0 and its configured name to the existing embedded `rgbwwctrl`; it must not allocate a collection on the heap. Other selectors return the standard light-not-found error.
- ESP32 owns a bounded collection of configured lights. Choose its storage after checking `APPLedCtrl` size and the target's actual PWM capabilities; avoid imposing that collection's memory cost on ESP8266.
- Select the architecture-specific provider through the Sming build/source layout instead of adding `#ifdef` branches throughout shared code.
- The accessors have the following shape:
  ```cpp
  APPLedCtrl* getLight(int idx = 0);
  APPLedCtrl* getLight(const String& name);
  ```
- Keep the legacy `rgbwwctrl` path for existing single-light callers during migration.
- `init()` uses the architecture-specific provider to initialize one or multiple configured lights.

### Step 5: Shared 50 Hz timer *(parallel with step 4)*
- Move `_ledTimer` from `APPLedCtrl` to `Application`; single callback iterates all lights, calling `updateLed()` on each — keeps all strips frame-synchronized

---

## Phase B — API Routing *(depends on Phase A)*

### Step 6: `JsonProcessor` light resolution
- Add `APPLedCtrl* resolveLight(JsonObject& root)` in `app/jsonprocessor.cpp`:
  - Resolves `root["light"]` (integer index or string name)
  - Missing/null → returns `lights[0]`
- Preserve existing unindexed request and response shapes; the optional selector only changes the target light.
- Replace all `app.rgbwwctrl.XXX()` calls with `resolveLight(root)->XXX()`
- Keep `serializeState()` unchanged for legacy calls; include light identity only in the new multi-light enumeration/explicitly indexed surfaces.

### Step 7: Webserver pass-through *(parallel with step 6)*
- Existing endpoints in `app/webserver.cpp` keep their current request/response contracts and accept an optional `light` query or body selector.
- Keep `GET /info` in its legacy single-light response shape; add a separate `/lights` endpoint for enumeration and per-light metadata/state.

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
  4. Validate the requested outputs against the selected hardware provider's available PWM channels and valid pins for the exact target/device; reject configurations that cannot be packed into the fixed banks
  5. Append the new light to the active pinconfig and persist via ConfigDB
  6. Return the created light and `restart_required: true`; do not instantiate a controller or change PWM/MQTT state in the running process
- Apply the same boot-time rule to deletion: update persistent pinconfig and per-light color data, then report that a restart is required. MQTT topics and controllers are recreated from the saved configuration on boot. The management layer may schedule a restart after the response is sent; live mutation is out of scope.
- Companion `DELETE /light/<name>` (or `delete_light` RPC) to remove a light:
  1. Remove it from ConfigDB hardware config and delete `last-colors[name]` from AppData
  2. **Constraint**: cannot delete the last remaining light (must always have at least one)
  3. Return `restart_required: true`; do not re-index or tear down active PWM while running
- Schema file `api-schemas/light.schema.json` to be created describing `create_light` and `delete_light` request bodies

---

## Phase C — Pub/Sub *(parallel with Phase B)*

### Step 8: EventServer multi-light
- Preserve the legacy default-light event payload. Add light identity/routing for multi-light events without requiring it from existing consumers.

### Step 9: MQTT multi-light topics
- `app/mqtt.cpp`: all publish/subscribe topics parameterized with light name
- Preserve current base topics as aliases for the default light; add `/rgbww/<light_name>/color` and `/rgbww/<light_name>/set` for explicit light routing.
- Incoming message: resolve the topic to `app.getLight(name)`; ESP8266 accepts only its configured light.

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
5. `GET /info` → retains the legacy single-light response; `GET /lights` → returns both lights, types, and current states
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
- **PWM limits**: queried from the selected hardware provider/device profile; do not hard-code one channel ceiling for an entire architecture
- **Target platforms**: ESP32, ESP32-S3, ESP32-C3, ESP32-C2, ESP8266, RP2040
- **API default**: missing `light` → light 0 (fully backward compatible)
- **MQTT topics**: name-based (`/rgbww/<light_name>/...`)

## Further Considerations

1. **ESP8266 output limits**: Preserve the current single-light behavior and validate pins through its hardware provider; do not add heap-backed per-light storage.
2. **RP2040**: Confirm PWM channel/pin capabilities through its provider rather than relying on a global schema limit.
3. **cfgdb migration risk**: Test `last-color` → `last-colors` migration carefully before removing the old key, as ConfigDB schema library behaviour with unknown keys needs verification.
4. **Note**: `app/ledctrl.cpp` already contains a `// ToDo:` comment describing this exact feature, confirming the intent was always planned.
