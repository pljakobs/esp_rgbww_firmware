# ESP RGBWW Firmware — API Reference

> This file is an index.  
> The full documentation has been split into three focused files:
>
> | File | Contents |
> |---|---|
> | [API_FUNCTIONS.md](API_FUNCTIONS.md) | What each function does, its parameters and return values (protocol-agnostic) |
> | [API_HTTP.md](API_HTTP.md) | HTTP REST specifics — endpoints, methods, request/response format, auth, CORS |
> | [API_WEBSOCKET.md](API_WEBSOCKET.md) | WebSocket specifics — JSON-RPC framing, auto-detection, streaming protocol |

---

## Quick Overview

The firmware exposes two access protocols:

| Protocol | Address | Notes |
|---|---|---|
| HTTP REST | `http://<device-ip>` | Stateless, one request per operation |
| WebSocket | `ws://<device-ip>/ws` | Persistent connection, JSON-RPC 2.0, streaming exports |

Both share the same function set. Authentication (HTTP Basic Auth) and CORS (`*`) are handled at the HTTP layer; the WebSocket connection inherits the same auth check on the upgrade request.

---

## Function Summary

| Function | Query | Command | Notes |
|---|---|---|---|
| `ping` | ✓ | — | HTTP only |
| `info` | ✓ | — | |
| `color` | ✓ | ✓ | |
| `on` | — | ✓ | |
| `off` | — | ✓ | |
| `toggle` | — | ✓ | |
| `stop` | — | ✓ | |
| `skip` | — | ✓ | |
| `pause` | — | ✓ | |
| `continue` | — | ✓ | |
| `blink` | — | ✓ | |
| `direct` | — | ✓ | WebSocket only |
| `networks` | ✓ | ✓ | Command = trigger scan |
| `connect` | ✓ | ✓ | HTTP only |
| `hosts` | ✓ | — | HTTP only |
| `system` | — | ✓ | |
| `update` | ✓ | ✓ | HTTP only |
| `config` | ✓ | ✓ | WS = read-only (streaming); write via HTTP POST |
| `data` | ✓ | ✓ | WS = read-only (streaming); write via HTTP POST |



**Success**
```json
{ "success": true }
```

**Error**
```json
{ "error": "error message" }
```
HTTP status is `200` for success and `400` for errors.  
Endpoints that return data respond with a plain JSON object (no `success` wrapper).

---

### Device

#### `GET /ping`
Keepalive / connectivity check.

**Response**
```json
{ "ping": "pong" }
```

---

#### `GET /info`
Returns firmware version, hardware details, WiFi connection state, and runtime stats.

**Response**
```json
{
  "deviceid": 12345678,
  "current_rom": "rom0",
  "git_version": "v1.2.3",
  "build_type": "release",
  "git_date": "2024-01-01",
  "webapp_version": "2.0.0",
  "sming": "5.0.0",
  "uptime": 3600,
  "cpu_usage_percent": 12,
  "heap_free": 32000,
  "soc": "esp8266",
  "rgbww": { "version": "1.0", "queuesize": 8 },
  "connection": {
    "connected": true,
    "ssid": "MyNetwork",
    "dhcp": true,
    "ip": "192.168.1.100",
    "netmask": "255.255.255.0",
    "gateway": "192.168.1.1",
    "mac": "AA:BB:CC:DD:EE:FF"
  }
}
```

---

#### `GET /update`
Returns the OTA update status.

**Response**
```json
{ "status": 0 }
```

| Status value | Meaning |
|---|---|
| `0` | Idle |
| `1` | Downloading |
| `2` | Flashing |
| `3` | Done / rebooting |

#### `POST /update`
Triggers an OTA firmware update.

**Request body**
```json
{
  "rom": { "url": "http://update-server.example/firmware.bin" }
}
```

---

### Color Control

#### `GET /color`
Returns the current color state.

**Response**
```json
{
  "hsv": { "h": 1.047, "s": 1.0, "v": 0.8, "ct": 0 },
  "raw": { "r": 0, "g": 0, "b": 0, "ww": 0, "cw": 0 }
}
```

`h`, `s`, `v` are in radians / 0–1 scale.  
`ct` is colour temperature in Kelvin (0 = not set, 100–500 = warm, 2000–10000 = full range).

---

#### `POST /color`
Sets the colour. All fields are optional — omit channels you do not want to change.

**HSV mode**
```json
{
  "hsv": { "h": 1.047, "s": 1.0, "v": 0.8, "ct": 0 },
  "cmd":  "fade",
  "t":    1000,
  "q":    "front"
}
```

**Raw PWM mode**
```json
{
  "raw": { "r": 512, "g": 0, "b": 1023, "ww": 0, "cw": 0 },
  "cmd": "solid"
}
```

**Common parameters**

| Field | Type | Default | Description |
|---|---|---|---|
| `cmd` | string | `"fade"` | `"fade"` or `"solid"` |
| `t` | int (ms) | — | Ramp/fade duration in milliseconds |
| `speed` | int | — | Ramp speed (alternative to `t`; cannot be 0) |
| `q` | string | — | Queue policy: `"front"`, `"back"`, `"single"`, `"back_reset"` |
| `requeue` | bool | `false` | Re-add the animation after completion |
| `direction` | int | `0` | `0` = shortest path, `1` = reverse |
| `channels` | array | all | Array of channels to affect, e.g. `["r","g","b","ww","cw"]` |
| `name` | string | — | Optional name tag for the animation |

**Response** — success: `{"success": true}`  

---

### Animation Control

All of the following accept `POST` only.  
The optional JSON body may contain a `channels` array to restrict which channels are affected.

```json
{ "channels": ["r", "g", "b"] }
```

| Endpoint | Effect |
|---|---|
| `POST /on` | Fade / set channels on (full brightness) |
| `POST /off` | Fade / set channels off (zero brightness) |
| `POST /toggle` | Toggle between on and off |
| `POST /stop` | Clear the animation queue and reset channels |
| `POST /skip` | Skip the currently running animation, start next |
| `POST /pause` | Pause the current animation |
| `POST /continue` | Resume a paused animation |

#### `POST /blink`
Trigger a blink animation.

```json
{ "t": 500, "q": "single" }
```

All return `{"success": true}` on success.

---

### Network

#### `GET /networks`
Returns the most recent WiFi scan results (cached).

```json
{
  "scanning": false,
  "available": [
    { "id": 1234, "ssid": "MyNetwork", "signal": -65, "encryption": "WPA2" }
  ]
}
```

If a scan is currently running, `scanning` is `true` and `available` is absent.

---

#### `POST /scan_networks`
Triggers a new WiFi scan. Returns immediately.

```json
{ "success": true }
```

Poll `GET /networks` until `scanning` is `false` to get results.

---

#### `GET /connect`
Returns the current WiFi connection status.

```json
{
  "status": 2,
  "ip": "192.168.1.100",
  "dhcp": "True",
  "ssid": "MyNetwork"
}
```

| Status | Meaning |
|---|---|
| `0` | Idle |
| `1` | Connecting |
| `2` | Connected |
| `3` | Error |

When status is `3`, an `"error"` field is also included.

---

#### `POST /connect`
Connect to a WiFi network.

```json
{ "ssid": "MyNetwork", "password": "secret" }
```

Returns `{"success": true}` immediately; poll `GET /connect` to track progress.

---

#### `GET /hosts`
Returns a list of discovered mDNS hosts (peer RGBWW controllers on the network).

Query parameters:
- `?all=1` — include incomplete / offline entries
- `?debug=1` — same as `all=1`

Response is a JSON array of host objects (streamed).

---

### Configuration Stores

The firmware has two ConfigDB stores:

| Store | Endpoint | Contents |
|---|---|---|
| `cfg` | `/config` | Device settings (network, color mode, calibration, …) |
| `data` | `/data` | Application data (presets, groups, controllers, scenes) |

Both endpoints work identically.

---

#### `GET /config` and `GET /data`
Returns the full store as a streamed JSON export.

```
GET /data
→ 200 application/json
← { "presets": [...], "groups": [...], "controllers": [...], "scenes": [...] }
```

---

#### `POST /config` and `POST /data`
Update one or more keys in the store.

The body is a flat JSON object whose keys are **ConfigDB selector strings** and whose values are the data to write.

**Selector syntax**

| Selector | Effect |
|---|---|
| `"presets[]"` | Append a new item to the `presets` array |
| `"presets[id=abc]"` | Update the fields of the item with `id == "abc"` (partial update) |
| `"presets[id=abc]"` → `[]` | Delete the item with `id == "abc"` by setting it to an empty array |
| `"connection.ssid"` | Set a scalar field via dot-notation |

> **Important:** Dot-notation inside a collection selector (`[id=x].field`) is **not supported**. Use an object value instead:
> ```json
> { "presets[id=abc]": { "name": "New Name" } }
> ```

**Examples**

**Append a new preset**
```json
{
  "presets[]": [{
    "id": "preset-1",
    "name": "Sunset",
    "color": { "hsv": { "h": 0.1, "s": 0.9, "v": 0.7, "ct": 0 } }
  }]
}
```

**Update a preset's name**
```json
{ "presets[id=preset-1]": { "name": "New Sunset" } }
```

**Delete a preset**
```json
{ "presets[id=preset-1]": [] }
```

**Response** — success: `{"success": true}`  
On error: `{"error": "description"}` with HTTP 400.

> Note: The device may close the connection before sending a complete response body on some firmware builds. Treat a `ChunkedEncodingError` or empty body as a success if the HTTP status is 200.

---

### System

#### `POST /system`
Execute a system-level command.

**Enable / disable debug logging**
```json
{ "cmd": "debug", "enable": true }
```

**Restart the device**
```json
{ "cmd": "restart" }
```

Returns `{"success": true}`.

---

## WebSocket API

### Protocol

- **Format**: [JSON-RPC 2.0](https://www.jsonrpc.org/specification)
- All messages are UTF-8 text frames (except binary chunk data — see [Streaming](#streaming)).

**Request**
```json
{ "jsonrpc": "2.0", "id": 1, "method": "color", "params": {} }
```

**Success response**
```json
{ "jsonrpc": "2.0", "id": 1, "result": "OK" }
```
or, for queries that return data:
```json
{ "jsonrpc": "2.0", "id": 1, "result": { ... } }
```

**Error response**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": { "code": -32603, "message": "error description" }
}
```

| Error code | Meaning |
|---|---|
| `-32601` | Method not found or not allowed as a command/query |
| `-32603` | Internal error / bad parameters |

**Notifications** (no `id`) are accepted but not replied to.

---

### Query vs. Command auto-detection

The server auto-detects whether a message is a *Query* (read) or *Command* (write):

- If `params` is absent or empty **and** the method is one of `info`, `status`, `networks`, `color`, `system`, `config`, `data` → treated as a **Query**.
- All other cases → treated as a **Command**.

---

### Methods

#### `color` — Query
Returns the current colour (same shape as `GET /color`).

```json
// request
{ "jsonrpc": "2.0", "id": 1, "method": "color" }

// response
{ "jsonrpc": "2.0", "id": 1, "result": {
    "hsv": { "h": 1.047, "s": 1.0, "v": 0.8, "ct": 0 },
    "raw": { "r": 0, "g": 0, "b": 0, "ww": 0, "cw": 0 }
}}
```

---

#### `color` — Command
Sets colour. Same parameters as `POST /color`.

```json
{ "jsonrpc": "2.0", "id": 2, "method": "color",
  "params": { "hsv": { "h": 0.5, "s": 1.0, "v": 0.8 }, "t": 1000 } }
```

---

#### `info` — Query only

```json
{ "jsonrpc": "2.0", "id": 3, "method": "info" }
```

Returns the same structure as `GET /info`.

---

#### `networks` — Query
Returns the cached scan result.

```json
{ "jsonrpc": "2.0", "id": 4, "method": "networks" }
→ result: { "scanning": false, "available": [...] }
```

#### `networks` — Command
Triggers a new scan.

```json
{ "jsonrpc": "2.0", "id": 5, "method": "networks", "params": { "scan": true } }
```

---

#### Animation control — Command only

All of these accept the same optional `channels` array as their HTTP counterparts.

| Method | Effect |
|---|---|
| `on` | Turn on |
| `off` | Turn off |
| `toggle` | Toggle on/off |
| `stop` | Clear queue and reset |
| `skip` | Skip current animation |
| `pause` | Pause |
| `continue` | Resume |
| `blink` | Blink — params: `{ "t": 500, "q": "single" }` |

Example:
```json
{ "jsonrpc": "2.0", "id": 10, "method": "stop",
  "params": { "channels": ["r","g","b"] } }
```

---

#### `direct` — Command only

Send raw PWM values without animation.

```json
{ "jsonrpc": "2.0", "id": 11, "method": "direct",
  "params": { "raw": { "r": 0, "g": 0, "b": 1023, "ww": 0, "cw": 0 } } }
```

---

#### `system` — Command only

```json
{ "jsonrpc": "2.0", "id": 20, "method": "system",
  "params": { "cmd": "debug", "enable": false } }

{ "jsonrpc": "2.0", "id": 21, "method": "system",
  "params": { "cmd": "restart" } }
```

---

#### `config` and `data` — Streaming Query

These methods export the full ConfigDB store over the WebSocket using a three-phase streaming protocol (see below).

```json
{ "jsonrpc": "2.0", "id": 30, "method": "data" }
```

No data can be written via the WebSocket `data`/`config` methods. To write, use `POST /data` or `POST /config` over HTTP.

---

### Streaming Protocol

Large data exports (`config` and `data`) are sent in multiple frames:

1. **TEXT frame** — stream start signal  
   ```json
   { "stream": "start", "id": 30 }
   ```

2. **One or more BINARY frames** — raw JSON chunk(s)  
   Each chunk is a UTF-8 bytes payload that forms part of the complete JSON document. Concatenate all chunks to reassemble the full JSON.

3. **TEXT frame** — stream end signal  
   ```json
   { "stream": "end", "id": 30 }
   ```

The `id` in the stream markers matches the `id` from the original JSON-RPC request, so multiple concurrent streams can be distinguished.

**Example (Python)**
```python
ws.send('{"jsonrpc":"2.0","id":30,"method":"data"}')

chunks = []
while True:
    msg = ws.recv()
    if isinstance(msg, bytes):
        chunks.append(msg)
    else:
        data = json.loads(msg)
        if data.get("stream") == "end":
            break

full_json = json.loads(b"".join(chunks))
```

---

## ConfigDB Data Store Schema

### `presets`
Array of colour presets.

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier |
| `name` | string | Display name |
| `color` | object | Color parameters (same as `POST /color` body) |

### `groups`
Array of controller groups.

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier |
| `name` | string | Display name |
| `controllerIds` | array | List of controller IDs in this group |

### `controllers`
Array of individual RGBWW controllers.

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier |
| `name` | string | Display name |
| `host` | string | Hostname or IP address |

### `scenes`
Array of saved scenes (multi-controller colour states).

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier |
| `name` | string | Display name |
| `steps` | array | Array of `{ controllerId, color }` steps |

---

## Configuration Store Schema (`/config`)

The `cfg` store contains device-level settings. Key sections:

| Section | Description |
|---|---|
| `connection` | WiFi SSID, password, DHCP, static IP |
| `color` | Output mode (HSV / Raw), channel mapping, calibration |
| `security` | HTTP Basic Auth credentials |
| `telemetry` | Remote logging / telemetry settings |

Retrieve the live schema with `GET /config`.
