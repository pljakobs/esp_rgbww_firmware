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
-- verify color is reflected in websocket message (websocket should send a notification of changed color)
- set raw
-- verify color set (get /color)
-- verify color is reflected in websocket message

# /off
- [x] verify v set to 0 (get /color and verify v component is 0)

# /on
- [x] verify v is set back to previous value

# /info
- [x] verify complete info structure
- [x] verify time passes between to calls to get /info

# /config
- [x] verify configuration can be read
- verify complete and partial updates can be written (documentation for selectors here https://github.com/mikee47/ConfigDB/)

# /data
- [x] verify data can be read
- [ ] verify complete and partial updates can be written 

- [ ] tests for all other endpoints according to the reference linked above

## websocket 
- [ ] cmd: color - set color and verify using httpb get to /color for both raw and hsv
- [ ] cmd: info - read info structure and verify it matches /info
- [ ] cmd: config - read config structure and verify it matches /config
