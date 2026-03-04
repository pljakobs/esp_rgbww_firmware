# ESP RGBWW Firmware — HTTP API

Protocol-specific details for the HTTP REST interface.  
For a description of what each function does see [API_FUNCTIONS.md](API_FUNCTIONS.md).

---

## Connection

| | |
|---|---|
| **Base URL** | `http://<device-ip>` |
| **Port** | 80 (default) |
| **Content-Type** | `application/json` for all POST bodies |

---

## Authentication

HTTP Basic Auth is supported. It is only enforced when credentials are configured on the device (via `/config`). Unauthenticated requests are accepted when no credentials are set.

---

## CORS

All endpoints send:

```
Access-Control-Allow-Origin: *
```

`OPTIONS` preflight requests are handled automatically.

---

## Response Format

### Data responses
Endpoints that return data respond with a plain JSON object and HTTP `200`:

```json
{ "ping": "pong" }
```

### Command responses — success
```json
{ "success": true }
```
HTTP `200`.

### Command responses — error
```json
{ "error": "error message" }
```
HTTP `400`.

> **Note**: Some POST endpoints (notably `/data` and `/config`) may close the TCP connection before sending a complete chunked response body on some firmware builds. Treat a connection-level error (`ChunkedEncodingError`) as a success when the HTTP status was `200`.

---

## Endpoint Reference

### Device

| Method | Path | Function | Description |
|---|---|---|---|
| `GET` | `/ping` | ping | Connectivity check |
| `GET` | `/info` | info | Firmware and runtime info |
| `GET` | `/update` | update (query) | OTA status |
| `POST` | `/update` | update (command) | Start OTA update |

---

### Color and Animation

| Method | Path | Function | Description |
|---|---|---|---|
| `GET` | `/color` | color (query) | Get current colour |
| `POST` | `/color` | color (command) | Set colour |
| `POST` | `/on` | on | Turn on |
| `POST` | `/off` | off | Turn off |
| `POST` | `/toggle` | toggle | Toggle on/off |
| `POST` | `/stop` | stop | Clear queue and reset |
| `POST` | `/skip` | skip | Skip current animation |
| `POST` | `/pause` | pause | Pause animation |
| `POST` | `/continue` | continue | Resume animation |
| `POST` | `/blink` | blink | Blink animation |

Animation control endpoints (`/on`, `/off`, `/toggle`, `/stop`, `/skip`, `/pause`, `/continue`, `/blink`) all accept an optional JSON body:

```json
{ "channels": ["r", "g", "b"] }
```

Omitting the body (or the `channels` field) affects all channels.

---

### Network

| Method | Path | Function | Description |
|---|---|---|---|
| `GET` | `/networks` | networks (query) | Cached scan results |
| `POST` | `/scan_networks` | networks (command) | Trigger a new scan |
| `GET` | `/connect` | connect (query) | WiFi connection status |
| `POST` | `/connect` | connect (command) | Connect to a network |
| `GET` | `/hosts` | hosts | mDNS peer list |

`GET /hosts` accepts optional query parameters:
- `?all=1` — include incomplete / offline entries
- `?debug=1` — same as `all=1`

---

### Configuration Stores

| Method | Path | Function | Description |
|---|---|---|---|
| `GET` | `/config` | config (query) | Export full device config |
| `POST` | `/config` | config (command) | Update device config keys |
| `GET` | `/data` | data (query) | Export full app data |
| `POST` | `/data` | data (command) | Update app data keys |

#### GET — streaming export
The response is a streamed `application/json` document containing the full store:

```
GET /data
← 200 application/json
← { "presets": [...], "groups": [...], "controllers": [...], "scenes": [...] }
```

#### POST — selector-based update
The body is a flat JSON object whose keys are ConfigDB selector strings.

```
POST /data
Content-Type: application/json

{
  "presets[]": [{ "id": "p1", "name": "Sunset", "color": { "hsv": { "h": 0.1, "s": 0.9, "v": 0.7, "ct": 0 } } }]
}
```

See [API_FUNCTIONS.md — ConfigDB Selector Syntax](API_FUNCTIONS.md#configdb-selector-syntax) for the full selector reference.

---

### System

| Method | Path | Function | Description |
|---|---|---|---|
| `GET` | `/system` | system (query) | (reserved / no-op) |
| `POST` | `/system` | system (command) | Execute system command |

```
POST /system
Content-Type: application/json

{ "cmd": "restart" }
```

---

## WebSocket Upgrade

| Method | Path | Description |
|---|---|---|
| `GET` | `/ws` | Upgrade to WebSocket connection |

The firmware handles the HTTP → WebSocket upgrade automatically.  
See [API_WEBSOCKET.md](API_WEBSOCKET.md) for the WebSocket API.
