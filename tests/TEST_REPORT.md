# API Refactoring Test Plan

This document outlines the testing strategy for the unified API refactoring of the ESP8266 Firmware.

## 1. Objective
To validate that the refactored `JsonProcessor` correctly handles requests from all three supported protocols with consistent logic.

## 2. Status Overview
- **HTTP API**: ✅ **PASSING** (All endpoints verified)
- **WebSocket API**: ⚠️ **PARTIAL** (Commands work, notifications work, Queries pending firmware update)
- **MQTT API**: ⏳ **PENDING**

## 3. Detailed Test Results (Latest Run)

### HTTP & Notifications
| Test Case | Method | Check | Result |
|-----------|--------|-------|--------|
| **Basics** | `GET /info` | Status 200, Keys (uptime, deviceid, heap_free) | ✅ PASS |
| **Config** | `GET /config` | Read Success | ✅ PASS |
| **Data** | `GET /data` | Read Success | ✅ PASS |
| **Color HSV** | `POST /color` | Set Blue (h=240) | ✅ PASS |
| | WS Notification | Received `color_event` | ✅ PASS |
| | Verify | `GET /color` -> h=240 | ✅ PASS |
| **Color RAW** | `POST /color` | Set Red (r=255) | ✅ PASS |
| | WS Notification | Received `color_event` | ✅ PASS |
| | Verify | `GET /color` -> r=255 | ✅ PASS |
| **Controls** | `POST /off` | Brightness (v) becomes 0 | ✅ PASS |
| | `POST /on` | Hue restored, Brightness restored | ✅ PASS |

### WebSocket API
| Test Case | Action | Expected | Actual | Status |
|-----------|--------|----------|--------|--------|
| **Connect** | Connect | Handshake | Connected | ✅ PASS |
| **Command** | `color` (Set) | `result: OK` | `result: OK` | ✅ PASS |
| **Query** | `color` (Get) | Color Object | Color Object | ✅ PASS |

## 4. Next Steps
1. **MQTT Testing**: Draft Phase 3 tests.
