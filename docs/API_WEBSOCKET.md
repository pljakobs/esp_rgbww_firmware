# ESP RGBWW Firmware — WebSocket API

Protocol-specific details for the WebSocket JSON-RPC interface.  
For a description of what each function does see [API_FUNCTIONS.md](API_FUNCTIONS.md).

---

## Connection

| | |
|---|---|
| **Endpoint** | `ws://<device-ip>/ws` |
| **Upgrade path** | `GET /ws` with standard WebSocket headers |
| **Frame encoding** | UTF-8 text (except binary data chunks — see [Streaming](#streaming)) |
| **Protocol** | [JSON-RPC 2.0](https://www.jsonrpc.org/specification) |

---

## Message Format

### Request

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "color",
  "params": { "hsv": { "h": 1.047, "s": 1.0, "v": 0.8 } }
}
```

- `id` — any integer; used to match the response. Omit for fire-and-forget notifications (no reply sent).
- `params` — omit or set to `{}` for query calls.

### Success Response

```json
{ "jsonrpc": "2.0", "id": 1, "result": "OK" }
```

When the method returns data, `result` is a JSON object instead of `"OK"`:

```json
{ "jsonrpc": "2.0", "id": 1, "result": { "hsv": { ... }, "raw": { ... } } }
```

### Error Response

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": { "code": -32603, "message": "error description" }
}
```

| Error code | Meaning |
|---|---|
| `-32601` | Method not found, or method called as wrong type (e.g. Query on a Command-only method) |
| `-32603` | Internal error / invalid parameters |

---

## Query vs. Command Auto-detection

The server infers whether a message is a *Query* (read) or *Command* (write) from the `params` field:

| Condition | Resolved as |
|---|---|
| `params` absent **or** empty (`{}`) **and** method is one of `info`, `status`, `networks`, `color`, `system`, `config`, `data` | **Query** |
| Any other case | **Command** |

This means:
- `{"method": "color"}` → query, returns current colour.
- `{"method": "color", "params": {"hsv": {...}}}` → command, sets colour.

---

## Method Reference

### Query-only methods

| Method | Returns |
|---|---|
| `info` | Full info object (see [API_FUNCTIONS.md — info](API_FUNCTIONS.md#info)) |
| `color` | Current colour state `{ hsv: {...}, raw: {...} }` |
| `networks` | `{ scanning: bool, available: [...] }` |

---

### Command-only methods

These methods return `"OK"` on success.

| Method | Params | Description |
|---|---|---|
| `on` | `{ channels?: [...] }` | Turn on |
| `off` | `{ channels?: [...] }` | Turn off |
| `toggle` | `{ channels?: [...] }` | Toggle on/off |
| `stop` | `{ channels?: [...] }` | Clear queue and reset |
| `skip` | `{ channels?: [...] }` | Skip current animation |
| `pause` | `{ channels?: [...] }` | Pause animation |
| `continue` | `{ channels?: [...] }` | Resume animation |
| `blink` | `{ t: ms, q: "single"\|..., channels?: [...] }` | Blink animation |
| `direct` | `{ raw: { r, g, b, ww, cw } }` | Raw PWM (bypasses animation system) |
| `networks` *(with params)* | any non-empty params | Trigger a new WiFi scan |
| `system` | `{ cmd: "debug"\|"restart", enable?: bool }` | System command |

Example — set colour with fade:
```json
{
  "jsonrpc": "2.0", "id": 10, "method": "color",
  "params": { "hsv": { "h": 0.5, "s": 1.0, "v": 0.8 }, "t": 1000, "q": "front" }
}
```

Example — trigger a scan:
```json
{
  "jsonrpc": "2.0", "id": 11, "method": "networks",
  "params": { "scan": true }
}
```

---

### Streaming methods

`config` and `data` export the full ConfigDB store using the [streaming protocol](#streaming) described below.

```json
{ "jsonrpc": "2.0", "id": 30, "method": "data" }
```

Streaming methods are **always** treated as queries — it is not possible to write `config` or `data` via WebSocket. Use `POST /config` or `POST /data` over HTTP for writes.

---

## Streaming

Large exports (`config` and `data`) are delivered in three phases using mixed text and binary frames:

### Phase 1 — Stream start (TEXT frame)

```json
{ "stream": "start", "id": 30 }
```

The `id` matches the `id` from the original request, allowing multiple concurrent streams to be distinguished.

### Phase 2 — Data chunks (BINARY frames)

Each frame is a raw UTF-8 byte payload forming part of the complete JSON document. Concatenate all binary frames in order to reconstruct the full document.

### Phase 3 — Stream end (TEXT frame)

```json
{ "stream": "end", "id": 30 }
```

After this frame the stream for that `id` is complete.

### Example (Python)

```python
import json, websocket

ws = websocket.create_connection("ws://192.168.1.100/ws")
ws.send(json.dumps({"jsonrpc": "2.0", "id": 30, "method": "data"}))

chunks = []
while True:
    opcode, data = ws.recv_data()
    if opcode == websocket.ABNF.OPCODE_BINARY:
        chunks.append(data)
    elif opcode == websocket.ABNF.OPCODE_TEXT:
        msg = json.loads(data)
        if msg.get("stream") == "end" and msg.get("id") == 30:
            break

full_data = json.loads(b"".join(chunks))
ws.close()
```

---

## Notifications (fire-and-forget)

The server may send unsolicited text frames to all connected clients when significant state changes occur (e.g. colour mode change requiring a restart). These messages are **not** JSON-RPC responses — they have the form:

```json
{ "notification": "Color Mode changed" }
```

Clients should be prepared to receive and discard (or display) these messages at any time.

---

## Concurrency

Multiple requests can be in-flight simultaneously. Responses are matched to requests via the `id` field. Streaming responses interleave binary chunk frames; use the `id` in the `stream_start`/`stream_end` frames to demultiplex.
