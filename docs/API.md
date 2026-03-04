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



