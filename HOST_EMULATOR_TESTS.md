# Host Emulator Smoke Tests - Comprehensive Guide

## Overview

The enhanced Host emulator smoke test suite (`host_ci_smoke_test.sh`) provides comprehensive validation of the API, WebSocket protocol, mDNS integration, and web interface. Results are automatically displayed in GitHub Actions workflows with detailed reporting.

## Test Categories

### 1. **Basic Connectivity**
- **Endpoint**: `/ping`
- **Purpose**: Validates Host emulator is ready and responsive
- **Expected**: `{"ping": "pong"}`

### 2. **WebSocket Protocol Validation**
- **RFC 6455 Compliance**: Full handshake with `Sec-WebSocket-Key` and `Sec-WebSocket-Accept` validation
- **JSON-RPC 2.0 Support**: Request-response correlation via `id` field and async event frames (notify pattern)
- **Frame Encoding**: Client-to-server masking, server-to-client unmasked frames
- **Tests**:
  - RFC 6455 handshake validation
  - JSON-RPC correlation with ID matching
  - Async event listener with timeout handling

### 3. **HTTP API Endpoint Coverage**

#### Getter Endpoints (GET)
- `/ping` - Basic connectivity check
- `/info` - Device and application information
- `/color` - Current color state (raw RGB/RGBWW and HSV)
- `/networks` - Available WiFi networks
- `/hosts` - **NEW:** mDNS-discovered hosts and services
- `/config` - Configuration settings
- `/data` - System data and statistics
- `/connect` - Connection status and parameters

#### Setter/Command Endpoints (POST)
- `/color` - Set color directly with round-trip verification
- `/on` - Turn lights on with event subscription
- `/off` - Turn lights off with event subscription
- `/stop`, `/skip`, `/pause`, `/continue` - Animation controls
- `/blink`, `/toggle` - Light effects

### 4. **Setter Functionality with Round-Trip Verification**
- **HTTP→WS Transparency**: Set via HTTP, read back via WebSocket
- **WS→HTTP Transparency**: Set via WebSocket, read back via HTTP
- **Validation**: Ensures state is consistent across transports
- **Example**: 
  ```
  HTTP POST /color with {r:12, g:34, b:56, ww:78, cw:90}
  → Poll HTTP /color until state changes
  → Verify via WS getColor RPC
  → Assert raw values match
  ```

### 5. **Error Handling**
- **Malformed JSON (HTTP)**: 
  - POST `/color` with invalid JSON
  - Expected: HTTP 400 + `{"error": "Invalid JSON"}`
  
- **Malformed JSON (WebSocket)**:
  - Send invalid JSON frame
  - Expected: `{"jsonrpc": "2.0", "id": <id>, "error": "..."}`
  
- **Missing Required Fields**:
  - WebSocket RPC without `method` field
  - Expected: Error response with request ID echoed

### 6. **Event Subscription (NEW)**
- **Event Source**: `/on` and `/off` commands trigger `color_event` notifications
- **Protocol**: WebSocket JSON-RPC 2.0 notify frames (no request `id`)
- **Payload**: `{"method": "color_event", "params": {"raw": {...}, "hsv": {...}}}`
- **Tests**:
  - POST `/off` → wait for `color_event` → verify all channels zero
  - POST `/on` → wait for `color_event` → verify at least one channel non-zero

### 7. **mDNS and Hosts Discovery (NEW)**
- **Endpoint**: `/hosts`
- **Purpose**: Returns mDNS-discovered hosts and their services
- **Response Type**: JSON object (structure depends on discovered services)
- **Test**: Validates endpoint responds with valid JSON

### 8. **Webapp Static Resources (NEW)**
- **Endpoints Tested**:
  - `/` - Root index page
  - `/webapp` - Web application interface
- **Validation**:
  - HTTP 200 status code
  - Valid HTML content (doctype/html tags)
  - Non-empty response
  - Error detection (catches error responses)
- **Purpose**: Ensures web UI is accessible alongside API

### 9. **Critical System Endpoints (NEW)**
- `/data` - System data endpoint
- `/connect` - Connection information
- **Validation**: All return valid JSON responses with 200 status

## Test Results Display

### Local Execution
```bash
.github/scripts/host_ci_smoke_test.sh
```

**Output Files**:
- `out/host-ci/host-smoke.log` - Full emulator log
- `out/host-ci/info.json` - Endpoint response samples
- `out/host-ci/test_summary.md` - Markdown summary report

### GitHub Actions Workflow

#### 1. **Annotations** (visible during workflow run)
```
::notice title=Host Emulator Smoke Test::All tests passed successfully
```

#### 2. **Group Sections** (collapsed/expandable)
```
::group::Test Summary
[Full markdown test report]
::endgroup::
```

#### 3. **Job Summary** (visible on workflow summary page)
The test results are automatically added to the GitHub Actions job summary, including:
- ✅ Test status indicators
- 📋 Coverage breakdown
- 🔧 Environment details (OS, Sming version, firmware version)
- 📄 Last 30 lines of application log

#### 4. **Artifacts** (downloadable after workflow)
- Artifact name: `host-smoke-{branch}`
- Retention: 30 days
- Contents:
  - `out/host-ci/` directory (all logs and reports)
  - Hardware config and partition table

### Interpreting Results

#### Success Indicator
```markdown
## Host Emulator Smoke Test Results
✅ **All tests passed**
```

#### Coverage Details
Each test category shows a ✅ indicator:
- ✅ WebSocket RFC 6455 handshake
- ✅ HTTP API endpoints (15+ endpoints)
- ✅ Setter transparency checks
- ✅ Event subscription
- ✅ mDNS hosts discovery
- ✅ Webapp static resources

#### Failure Scenarios
If a test fails, the script outputs:
- **Assertion message** describing what failed
- **Expected vs. actual** values
- **Context** (which endpoint, transport, etc.)

Example:
```
Transparency check failed: HTTP-set color differs between HTTP and WS reads
(http={'r': 12, 'g': 34, 'b': 56}, ws={'r': 255, 'g': 0, 'b': 0})
```

## Running Tests Locally

### Prerequisites
```bash
# TAP interface support (Linux)
sudo modprobe tun

# Sming environment
source /opt/sming/Tools/export.sh
```

### Execute
```bash
cd /home/pjakobs/devel/esp_rgbww_firmware
bash .github/scripts/host_ci_smoke_test.sh
```

### View Results
```bash
# Application log
tail -f out/host-ci/host-smoke.log

# Test summary
cat out/host-ci/test_summary.md

# Info samples
cat out/host-ci/info.json | jq
```

## Architecture: How Tests Work

### WebSocket Client (RFC 6455)
```
1. TCP connection to ${WS_HOST}:${WS_PORT}${WS_PATH}
2. Send HTTP upgrade request with Sec-WebSocket-Key
3. Receive HTTP 101 response with Sec-WebSocket-Accept (validated with SHA1)
4. Exchange frames with masking (client→server) and unmasked (server→client)
5. Send/receive JSON-RPC 2.0 messages with id correlation
6. Listen for async event frames (no id field)
```

### HTTP Client
```
1. Construct request URL: http://${APP_IP}${PATH}
2. Set Content-Type: application/json for POST with payload
3. Parse response JSON or return error
4. Retry logic for state-change tests (poll up to 20x with 0.1s sleep)
```

### State Transparency Testing
```
HTTP Setter → Poll HTTP Getter → Compare WS Getter
↓
Verify all three return identical state (raw channel values)
↓
Repeat in reverse (WS Setter → HTTP Getter → WS Getter)
```

### Event Subscription
```
Send Command (POST /off or /on)
↓
WebSocket listener waits for "color_event" method (no id field)
↓
Timeout after 8 seconds
↓
Validate event payload matches expected state
```

## Troubleshooting

### Test Failure: "Timed out waiting for Host API readiness"
**Cause**: Host emulator didn't start or crashed
**Fix**: 
1. Check `/dev/net/tun` exists: `ls -l /dev/net/tun`
2. Verify TAP interface: `ip link show tap0`
3. Review full log: `tail -n 200 out/host-ci/host-smoke.log`

### Test Failure: "Transparency check failed"
**Cause**: State read differs between HTTP and WebSocket
**Fix**:
1. Check `/color` response structure matches schema
2. Verify JSON-RPC id correlation in WebSocket messages
3. Check event publication timing in EventServer

### Test Failure: "Malformed JSON (HTTP)"
**Cause**: Error response format incorrect
**Fix**:
1. Verify `parseJsonBody()` in `app/webserver.cpp` returns `{"error": "Invalid JSON"}`
2. Ensure HTTP 400 status code on JSON error
3. Check Content-Type header handling

### Test Failure: "mDNS hosts endpoint"
**Cause**: `/hosts` endpoint unavailable or returns wrong type
**Fix**:
1. Verify `ApiHandler::getHosts()` implementation
2. Check mDNS discovery is enabled in host config
3. Review `/hosts` endpoint registration in `ApplicationWebserver::init()`

### Test Failure: "Webapp tests"
**Cause**: `/` or `/webapp` endpoint returns non-HTML or 404
**Fix**:
1. Verify webapp files are embedded in firmware
2. Check file mappings in `app/fileMap.cpp`
3. Ensure static file serving is enabled in webserver config

## Integration with CI/CD

The Host smoke test is a **blocking prerequisite** for hardware builds:

```yaml
build:
  needs: [generate-matrix, host-smoke]  # ← host-smoke must pass
```

This ensures:
- ✅ API is functional before hardware deployment
- ✅ Webapp is accessible before release
- ✅ WebSocket protocol works correctly
- ✅ State transparency is verified
- ✅ mDNS integration is tested

## Future Enhancements

Potential additions to expand test coverage:

1. **Performance Testing**
   - Measure endpoint response times
   - Check WebSocket frame latency
   - Validate event delivery speed

2. **Stress Testing**
   - Multiple concurrent connections
   - Rapid command sequences
   - Large payload handling

3. **Network Resilience**
   - Connection recovery
   - Message ordering under load
   - Reconnection after network loss

4. **Webapp Functional Tests**
   - UI element visibility (via headless browser)
   - Color picker functionality
   - Animation controls

5. **mDNS Service Advertisement**
   - Inject mDNS announcements
   - Verify service discovery
   - Test TXT record parsing

6. **Configuration Persistence**
   - Verify ConfigDB integration
   - Test config reload functionality

## Related Files

- Test script: [.github/scripts/host_ci_smoke_test.sh](.github/scripts/host_ci_smoke_test.sh)
- Workflow: [.github/workflows/build_firmware.yml](.github/workflows/build_firmware.yml)
- API handler: [app/apihandler.cpp](app/apihandler.cpp)
- WebSocket handler: [app/webserver.cpp](app/webserver.cpp)
- Event server: [app/eventserver.cpp](app/eventserver.cpp)

## Questions?

For debugging or enhancements, check:
1. GitHub Actions run logs (full terminal output)
2. Uploaded artifact `host-smoke-{branch}` (contains all test logs)
3. Job summary page (shows final test report)
