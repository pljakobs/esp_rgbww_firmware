# ESP RGBWW Firmware — Function Reference

Describes every function exposed by the firmware, independent of which access protocol (HTTP or WebSocket) is used.
For protocol-specific calling conventions see [API_HTTP.md](API_HTTP.md) and [API_WEBSOCKET.md](API_WEBSOCKET.md).

---

## `ping`

Keepalive / connectivity check.

- **Type**: Query
- **Parameters**: none
- **Returns**:

```json
{ "ping": "pong" }
```

---

## `info`

Returns firmware and runtime information.

- **Type**: Query only
- **Parameters**: none
- **Returns**:

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

## `color`

### Query
Returns the current output colour.

- **Parameters**: none
- **Returns**:

```json
{
  "hsv": { "h": 1.047, "s": 1.0, "v": 0.8, "ct": 0 },
  "raw": { "r": 0, "g": 0, "b": 0, "ww": 0, "cw": 0 }
}
```

`h`, `s`, `v` are floating-point (radians / 0–1 scale).  
`ct` is colour temperature in Kelvin (0 = not set; valid range: 100–500 or 2000–10000).

### Command
Sets the output colour.

All colour fields and animation fields are optional — omit what you do not want to change.

**HSV mode**
```json
{
  "hsv": { "h": 1.047, "s": 1.0, "v": 0.8, "ct": 0 }
}
```

**Raw PWM mode**
```json
{
  "raw": { "r": 512, "g": 0, "b": 1023, "ww": 0, "cw": 0 }
}
```

**Animation parameters** (applicable to both modes)

| Field | Type | Default | Description |
|---|---|---|---|
| `cmd` | string | `"fade"` | `"fade"` for a smooth transition, `"solid"` for instant |
| `t` | int (ms) | — | Ramp / fade duration in milliseconds |
| `speed` | int | — | Ramp speed (alternative to `t`; must not be 0) |
| `q` | string | — | Queue policy: `"front"`, `"back"`, `"single"`, `"back_reset"` |
| `requeue` | bool | `false` | Re-add animation to queue after it completes |
| `direction` | int | `0` | `0` = shortest hue path, `1` = reverse direction |
| `channels` | array | all | Which channels to affect, e.g. `["r","g","b","ww","cw"]` |
| `name` | string | — | Optional label for this animation entry |

**Returns**: success / error.

---

## `on`

Fades or sets the specified channels to full brightness.

- **Type**: Command
- **Parameters**: optional `channels` array
- **Returns**: success / error

---

## `off`

Fades or sets the specified channels to zero brightness. If no colour is specified and the device is in HSV mode, the current hue and saturation are preserved and only the value is set to 0.

- **Type**: Command
- **Parameters**: optional `channels` array; optionally any `color` parameters to define the target off-state
- **Returns**: success / error

---

## `toggle`

Toggles the output between on and off.

- **Type**: Command
- **Parameters**: optional `channels` array
- **Returns**: success / error

---

## `stop`

Clears the animation queue and immediately resets the specified channels.

- **Type**: Command
- **Parameters**: optional `channels` array
- **Returns**: success / error

---

## `skip`

Skips the currently running animation and starts the next one in the queue.

- **Type**: Command
- **Parameters**: optional `channels` array
- **Returns**: success / error

---

## `pause`

Pauses the current animation. The animation state is preserved.

- **Type**: Command
- **Parameters**: optional `channels` array
- **Returns**: success / error

---

## `continue`

Resumes a paused animation.

- **Type**: Command
- **Parameters**: optional `channels` array
- **Returns**: success / error

---

## `blink`

Triggers a blink animation.

- **Type**: Command
- **Parameters**:

| Field | Type | Description |
|---|---|---|
| `t` | int (ms) | Duration of one blink cycle |
| `q` | string | Queue policy (e.g. `"single"`) |

- **Returns**: success / error

---

## `direct`

Writes raw PWM values immediately, bypassing the animation system.

- **Type**: Command (WebSocket only)
- **Parameters**:

```json
{ "raw": { "r": 0, "g": 0, "b": 1023, "ww": 0, "cw": 0 } }
```

- **Returns**: success / error

---

## `networks`

### Query
Returns the cached WiFi scan result.

- **Parameters**: none
- **Returns**:

```json
{
  "scanning": false,
  "available": [
    { "id": 1234, "ssid": "MyNetwork", "signal": -65, "encryption": "WPA2" }
  ]
}
```

If a scan is running, `"scanning"` is `true` and `"available"` is absent.

### Command (`scan_networks`)
Triggers a new WiFi scan. Returns immediately; poll `networks` (query) until `scanning` is `false`.

- **Parameters**: none (or `{ "scan": true }` via WebSocket)
- **Returns**: success / error

---

## `connect`

### Query
Returns the current WiFi connection state.

- **Returns**:

```json
{
  "status": 2,
  "ip": "192.168.1.100",
  "dhcp": "True",
  "ssid": "MyNetwork"
}
```

| Status value | Meaning |
|---|---|
| `0` | Idle |
| `1` | Connecting |
| `2` | Connected |
| `3` | Error (also includes `"error"` field) |

### Command
Initiates a connection to the given network.

- **Parameters**:

```json
{ "ssid": "MyNetwork", "password": "secret" }
```

- **Returns**: success / error (immediate; poll query to track progress)

---

## `system`

Execute a system-level command.

- **Type**: Command
- **Parameters**:

**Toggle debug logging**
```json
{ "cmd": "debug", "enable": true }
```

**Restart the device**
```json
{ "cmd": "restart" }
```

- **Returns**: success / error

---

## `update`

### Query
Returns the OTA update status.

- **Returns**:

```json
{ "status": 0 }
```

| Status value | Meaning |
|---|---|
| `0` | Idle |
| `1` | Downloading |
| `2` | Flashing |
| `3` | Done / rebooting |

### Command
Starts an OTA firmware update.

- **Parameters**:

```json
{ "rom": { "url": "http://update-server.example/firmware.bin" } }
```

- **Returns**: success / error

---

## `hosts`

Returns discovered mDNS peers (other RGBWW controllers on the local network).

- **Type**: Query (HTTP only)
- **Parameters** (HTTP query string):
  - `?all=1` — include incomplete / offline entries
  - `?debug=1` — same as `all=1`
- **Returns**: JSON array of host objects (streamed)

---

## `config`

Full export or update of the **device configuration** store (network settings, colour mode, calibration, security, etc.).

### Query
Returns the full cfg store as a JSON document.

### Command / Update
Writes one or more keys using ConfigDB selector syntax.

See [ConfigDB Selector Syntax](#configdb-selector-syntax) below and [API_HTTP.md](API_HTTP.md) for write details.

---

## `data`

Full export or update of the **application data** store (presets, groups, controllers, scenes).

### Query
Returns the full data store as a JSON document.

### Command / Update
Writes one or more keys using ConfigDB selector syntax.

---

## ConfigDB Selector Syntax

Both `config` and `data` POST bodies are flat JSON objects whose keys are selector strings.

| Selector | Value | Effect |
|---|---|---|
| `"presets[]"` | `[{...}]` | Append a new item to the collection |
| `"presets[id=abc]"` | `{partial object}` | Update fields on the item with `id == "abc"` |
| `"presets[id=abc]"` | `[]` | Delete the item with `id == "abc"` |
| `"connection.ssid"` | `"MySSID"` | Set a scalar field via dot-notation |

> **Limitation**: Dot-notation **inside** a collection selector (`[id=x].field`) is not supported.  
> Always use an object value for partial updates:
> ```json
> { "presets[id=abc]": { "name": "New Name" } }
> ```

### Data store collections

#### `presets`

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier |
| `name` | string | Display name |
| `color` | object | Color params (same shape as `color` command) |

#### `groups`

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier |
| `name` | string | Display name |
| `controllerIds` | array | List of controller IDs belonging to this group |

#### `controllers`

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier |
| `name` | string | Display name |
| `host` | string | Hostname or IP address of the controller |

#### `scenes`

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier |
| `name` | string | Display name |
| `steps` | array | `[{ "controllerId": "...", "color": {...} }, ...]` |
