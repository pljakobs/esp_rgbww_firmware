# API Refactoring Test Plan

This document outlines the testing strategy for the unified API refactoring of the ESP8266 Firmware. The goal is to ensure consistent behavior across HTTP, WebSocket, and MQTT interfaces handling command and state logic via the unified `JsonProcessor`.

## 1. Objective
To validate that the refactored `JsonProcessor` correctly handles requests from all three supported protocols with consistent logic, verifying:
- **Command Execution**: Changing colors, switching on/off.
- **State Querying**: Retrieving current status/color.
- **Protocol Compliance**: Maintaining backward compatibility where needed (HTTP) and standard compliance (JSON-RPC 2.0).

## 2. Prerequisites
- **Firmware**: Must be flashed with the latest `JsonProcessor.cpp` containing:
    - Unified `handleRequest` method.
    - Auto-detection logic for Query vs Command based on empty parameters.
- **Environment**:
    - Python 3.x
    - Virtual Environment with dependencies: `requests`, `websocket-client`, `toml`.
    - Network Access to the ESP device.

## 3. Test Phases

### Phase 1: HTTP API (Completed)
**Goal**: Verify the REST-like interface functionality.

| Endpoint | Method | Purpose | Expected Result |
|----------|--------|---------|-----------------|
| `/info` | `GET` | System Status | JSON with uptime, heap, etc. 200 OK. |
| `/color` | `GET` | Get Color | JSON with `raw` (RGB) & `hsv`. 200 OK. |
| `/color` | `POST` | Set Color | Update LED state. 200 OK. |
| `/on` | `GET/POST` | Turn On | State becomes true. 200 OK. |
| `/off` | `GET/POST` | Turn Off | State becomes false. 200 OK. |

*Status*: **PASSING** (Verified via `test_api.py`)

### Phase 2: WebSocket API (Active)
**Goal**: Verify the JSON-RPC 2.0 interface for real-time control.

| Action | Method | Params | Expected Result |
|--------|--------|--------|-----------------|
| **Connect** | - | - | Successful Handshake (101). |
| **Query** | `color` | `{}` | Returns current color state (result object). |
| **Command** | `color` | `{"raw":{...}}` | Updates color, returns "OK" or new state. |
| **Verify** | `GET /color` | - | HTTP confirms change made via WS. |

*Status*: **PARTIAL**
- **Connect**: PASS
- **Command**: PASS
- **Query**: FAIL (Requires firmware update for empty-param detection logic)

### Phase 3: MQTT API (Planned)
**Goal**: Verify async messaging and Home Assistant integration.

*Note: This phase is deferred until WS validation is complete.*

| Topic | Logic | Expected Result |
|-------|-------|-----------------|
| `rgb/set` | Command | Device updates state. |
| `rgb/status` | State | Device publishes state on change. |
| `homeassistant/...` | Discovery | Auto-discovery config payload published. |

---

## 4. Execution Guide

### Setup
1. Create/Update configuration file `tests/test_config.toml`:
   ```toml
   [device]
   ip = "192.168.1.105"  # Replace with actual device IP
   ```

2. Activate environment:
   ```bash
   source .venv/bin/activate
   ```

### Running Tests
Execute the comprehensive test suite:
```bash
python tests/test_api.py --config tests/test_config.toml
```

### Analyzing Results
- **Success**: Logs will show `SUCCESS` for each step.
- **Failure**: Check `api_test_results.log` for detailed error messages and response bodies.
- **Common Issues**:
    - `WebSocket Error -32603`: "Internal Error" often means parameter mismatch or the firmware treating a Query as a Command (missing data).

## 5. Next Steps
1. **Flash Firmware**: Update device with latest `JsonProcessor` changes.
2. **Re-run Phase 2**: Confirm WebSocket Query now passes.
3. **Draft Phase 3**: Implement MQTT test cases in `test_api.py`.

### ToDo:

## http 

# full documentation: https://github.com/patrickjahns/esp_rgbww_firmware/wiki/2.1-JSON-API-reference

# /color
- [x] set hsv 
-- [x] verify color set (get /color)
-- [x] verify color is reflected in websocket message (websocket should send a notification of changed color)
- [x] set raw
-- [x] verify color set (get /color)
-- [x] verify color is reflected in websocket message

# /off
- [x] verify v set to 0 (get /color and verify v component is 0)
- [x] GET /off (GET method variant)

# /on
- [x] verify v is set back to previous value
- [x] GET /on (GET method variant)

# /info
- [x] verify complete info structure
- [x] verify time passes between to calls to get /info

# /config
- [x] verify configuration can be read
- [x] verify partial updates can be written (documentation for selectors here https://github.com/mikee47/ConfigDB/)

# /data
- [x] verify data can be read
- [x] verify partial updates can be written (sync-lock.id)
- [x] preset CREATE, field UPDATE, DELETE via array notation
- [x] group CREATE, field UPDATE, DELETE via array notation
- [x] controller CREATE, field UPDATE, DELETE via array notation
- [x] scene CREATE, field UPDATE, DELETE via array notation
- [x] type-error: float in stored hsv → FormatError::BadType (400)

# /ping
- [x] verify ping → pong

# /networks
- [x] verify structure (scanning flag + available list)

# /scan_networks
- [x] verify POST triggers scan and returns 200

# /hosts
- [x] GET /hosts (no params) — returns visible controllers list
- [x] GET /hosts?all=false — explicit visible-only filter, matches default
- [x] GET /hosts?all=true — returns all controllers (>= visible-only count)
- [x] GET /hosts?all=1 — numeric alias for all=true accepted by firmware

# /connect
- [x] GET /connect — status field present, value 0-3
- [x] POST /connect — missing ssid returns error (no network disruption)
- [x] POST /connect — empty ssid returns error

# /system
- [x] POST /system cmd:debug enable/disable

# /stop
- [x] POST /stop (no channels) — returns 200, device stays responsive
- [x] POST /stop (specific channels)

# /pause + /continue
- [x] POST /pause then POST /continue — both return 200
- [x] POST /pause with specific channels

# /skip
- [x] POST /skip — skips current animation, device stays responsive

# /blink
- [x] POST /blink — returns 200, WS color_event received

# /toggle
- [x] POST /toggle — first call sets v=0, second call restores brightness

# /update
- [x] GET /update — structure (rom_status, webapp_status) verified
- [ ] POST /update — NOT tested: any POST /update call (even with an invalid URL)
      puts the firmware into an "OTA in progress" state that persists until reboot,
      breaking all subsequent tests. Manual testing only.

## websocket 
- [x] cmd: color (HSV) - set color and verify using HTTP GET /color
- [x] cmd: color (RAW) - set color and verify using HTTP GET /color
- [x] cmd: color triggers color_event broadcast notification
- [x] cmd: info - read info structure and verify it matches /info
- [x] cmd: config - read config structure via streaming and verify it matches /config
- [x] cmd: data - read data structure via streaming and verify it matches /data
- [x] cmd: color (Query, empty params) - returns current color state
- [x] cmd: off - verify v=0 via GET /color
- [x] cmd: on - verify v>0 via GET /color
- [x] cmd: on/off sequence - round-trip via WS only
- [x] cmd: toggle (×2 cycle) - v→0 then v restored
- [x] cmd: stop (no channels) - device stays responsive
- [x] cmd: stop (specific channels)
- [x] cmd: pause + continue - both succeed, device stays alive
- [x] cmd: pause (specific channels)
- [x] cmd: skip - animation skipped, device responsive
- [x] cmd: blink - color_event broadcast received
- [x] cmd: networks (empty params / Query) - returns scanning + available list
- [x] cmd: networks (non-empty params / Command) - initiates scan
- [x] cmd: networks consistency with GET /networks
- [x] cmd: system {"cmd":"debug","enable":true} - succeeds
- [x] cmd: system {"cmd":"debug","enable":false} - succeeds
- [x] cmd: system (empty params) - auto-detected as Query → returns -32601 error
