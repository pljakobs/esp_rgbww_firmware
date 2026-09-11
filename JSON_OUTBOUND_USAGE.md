# Outbound JSON Construction Inventory

This document lists every place in the firmware (`app/`) where an ArduinoJson
document is **built for serialization and transmission** (HTTP response,
WebSocket frame, TCP event stream, or MQTT publish). Inbound/parsing-only uses
of `deserializeJson` are intentionally excluded, as are documents that are only
persisted to flash.

ArduinoJson v6 is used throughout (`ArduinoJson6` component, see
[component.mk](component.mk#L2)). Serialization goes through Sming's
`Json::serialize()` helper, a `JsonObjectStream`, or `serializeJsonPretty()`.

Locations are grouped by whether they can be migrated to the streaming
[`JsonWriter`](Components/JsonWriter/include/JsonWriter.h) component, which now
supports a `String` target (`JsonWriter(String&)`), a `Print`/stream target, and
self-buffering mode (usable directly as an `IDataSourceStream`).

**Legend**

- ✅ **Drop-in** — builds a document solely to serialize into a `String`/stream
  and send. Directly replaceable by `JsonWriter` with identical output.
- 🟡 **Refactor** — already streams, but the payload is populated through a
  shared `JsonObject` handed across layers; migrating requires rewriting the
  producers too.
- ❌ **Blocked** — passes or merges a live `JsonObject` (random-access / merge),
  which a forward-only writer cannot accept without changing the data model.

---

## ✅ Drop-in replacements (String target)

These build a `Static/DynamicJsonDocument` **only** to `Json::serialize()` into a
`String` and `publish()`. They map 1:1 onto `JsonWriter(String&)` and drop a
128–768 B `JsonDocument` off the CONT stack/heap on each call.

| Function | Location | Document | Topic |
|----------|----------|----------|-------|
| `publishCurrentRaw()` | [mqtt.cpp#L295](app/mqtt.cpp#L295) | `StaticJsonDocument<200>` | `.../color` |
| `publishCurrentHsv()` | [mqtt.cpp#L328](app/mqtt.cpp#L328) | `StaticJsonDocument<200>` | `.../color` |
| `publishTransitionFinished()` | [mqtt.cpp#L407](app/mqtt.cpp#L407) | `StaticJsonDocument<200>` | `.../transition_finished` |
| `publishHADiscovery()` | [mqtt.cpp#L482](app/mqtt.cpp#L482) | `DynamicJsonDocument(768)` | `homeassistant/light/.../config` |
| `publishChannelConfig()` | [mqtt.cpp#L564](app/mqtt.cpp#L564) | `DynamicJsonDocument(512)` | `.../<channel>/config` |
| `publishHAState()` | [mqtt.cpp#L611](app/mqtt.cpp#L611) | `StaticJsonDocument<256>` | `.../state` |
| `publishChannelState()` | [mqtt.cpp#L670](app/mqtt.cpp#L670) | `StaticJsonDocument<128>` | `.../<channel>/state` |

All serialize via `Json::serialize(...)` then `publish(topic, payload, retain)`.

The telemetry publish is a near-neighbour but sends the **document** (not a
serialized string) into `telemetryClient.stat(doc)`, so it depends on the
telemetry client accepting a `JsonWriter`/string payload:

| Function | Location | Document | Transport |
|----------|----------|----------|-----------|
| `checkRam()` | [application.cpp#L388](app/application.cpp#L388) | `DynamicJsonDocument(256)` | `telemetryClient.stat(doc)` (MQTT) |

The same tick already hand-builds its `runtime_info` WebSocket frame with
`m_snprintf` (no ArduinoJson) at [application.cpp#L426](app/application.cpp#L426).

---

## 🟡 Possible, but a real refactor (stream target)

These already stream, but their body is populated through a shared
`JsonObject`/`JsonObjectStream` handed across layers, so swapping the writer
means rewriting the producers (the `Api::handle*` layer and `addInfoFields()`)
to write into a `JsonWriter` scope.

### HTTP REST responses — [app/webserver.cpp](app/webserver.cpp)

All REST handlers stream their payload through a `JsonObjectStream`, whose root
object is populated and then handed to `response.sendDataStream(...)` /
`sendApiResponse(...)` (shared exit point
[webserver.cpp#L550](app/webserver.cpp#L550)).

| Function | Location | Document | Transport |
|----------|----------|----------|-----------|
| `sendApiCode()` (FlashHelper) | [webserver.cpp#L577](app/webserver.cpp#L577) | `JsonObjectStream` (default) | HTTP body (`success`/`error`/`info`) |
| `sendApiCode()` (char*) | [webserver.cpp#L664](app/webserver.cpp#L664) | `JsonObjectStream` (default) | HTTP body |
| `addInfoFields()` | [webserver.cpp#L614](app/webserver.cpp#L614) | fills caller `JsonObject` | nested into responses above |
| `onIndex()` (webapp version) | [webserver.cpp#L926](app/webserver.cpp#L926) | `JsonObjectStream(512)` | HTTP body |
| `onConfig()` | [webserver.cpp#L1397](app/webserver.cpp#L1397) | `JsonObjectStream` (configStream) | HTTP body |
| `onInfo()` | [webserver.cpp#L1443](app/webserver.cpp#L1443) | `JsonObjectStream(INFO_DOC_CAPACITY_V1/V2)` | HTTP body |
| `onNetworks()` | [webserver.cpp#L1615](app/webserver.cpp#L1615) | `JsonObjectStream` | HTTP body |
| `onConnect()` | [webserver.cpp#L1740](app/webserver.cpp#L1740) | `JsonObjectStream` | HTTP body |
| `onUpdate()` | [webserver.cpp#L1878](app/webserver.cpp#L1878) | `JsonObjectStream` | HTTP body |
| `onPing()` | [webserver.cpp#L1916](app/webserver.cpp#L1916) | `JsonObjectStream` | HTTP body |
| `onHosts()` | [webserver.cpp#L2082](app/webserver.cpp#L2082) | `JsonObjectStream` | HTTP body |
| `onData()` | [webserver.cpp#L2100](app/webserver.cpp#L2100) | `JsonObjectStream` | HTTP body |

### WebSocket JSON-RPC reply — [app/webserver.cpp](app/webserver.cpp)

| Function | Location | Document | Transport |
|----------|----------|----------|-----------|
| `wsMessage()` (RPC reply) | [webserver.cpp#L281](app/webserver.cpp#L281) | `JsonObjectStream(responseCapacity)` | `socket.send(stream, WS_FRAME_TEXT)` |

The `wsMessage()` response builds `jsonrpc`/`id`/`result`/`error` on
`responseRoot` ([webserver.cpp#L286](app/webserver.cpp#L286)); `result` bodies are
filled by the API handlers below.

Fast-path frames that already bypass ArduinoJson (pre-formatted strings) are
broadcast via `wsSendBroadcast()` / `wsSendRuntimeInfo()`
([webserver.cpp#L420](app/webserver.cpp#L420)) and need no migration.

### API result builders — [app/apihandler.cpp](app/apihandler.cpp)

These do not send directly; they populate the `out` `JsonObject` that becomes the
`result` of a WebSocket/HTTP response. They are the producers that must be
rewritten for any of the 🟡 HTTP/WS migrations above.

| Function | Location | Notes |
|----------|----------|-------|
| `handleColor()` | [apihandler.cpp#L285](app/apihandler.cpp#L285) | builds `raw` + `hsv` objects |
| `handleNetworks()` | [apihandler.cpp#L310](app/apihandler.cpp#L310) | builds `available` array |
| `handleInfo()` | see `handleInfo` in apihandler.cpp | builds device/app/runtime info |

---

## ❌ Blocked (passes / merges a live `JsonObject`)

These pass or merge an existing `JsonObject`, which a forward-only writer cannot
accept without changing the data model.

### Shared JSON-RPC envelope — [app/jsonrpcmessage.cpp](app/jsonrpcmessage.cpp)

`JsonRpcMessage` ([jsonrpcmessage.cpp#L24](app/jsonrpcmessage.cpp#L24)) wraps a
`DynamicJsonDocument(MAX_JSON_MESSAGE_LENGTH)` and stamps `jsonrpc`/`method`. It
exposes `getParams()`/`getRoot()` for random-access population, so every
consumer below is blocked on it.

### WebSocket broadcast helpers — [app/application.cpp](app/application.cpp)

| Function | Location | Document | Transport |
|----------|----------|----------|-----------|
| `wsBroadcast(cmd, message)` | [application.cpp#L1255](app/application.cpp#L1255) | `JsonRpcMessage` | `wsBroadcast(String)` → WebSocket |
| `wsBroadcast(cmd, params)` | [application.cpp#L1266](app/application.cpp#L1266) | `JsonRpcMessage` (merges `params`) | `wsBroadcast(String)` → WebSocket |

Call sites at [webserver.cpp#L1212](app/webserver.cpp#L1212) and nearby
(`notification`/`config` events).

### TCP Event Server — [app/eventserver.cpp](app/eventserver.cpp)

Pushes JSON-RPC event frames to subscribed TCP clients via `sendRoot()` →
`sendToClients()`. `_colorDoc` is a persistent static-pool document reused per
publish to avoid per-event heap allocation
([eventserver.cpp#L143](app/eventserver.cpp#L143)).

| Function | Location | Document | Transport |
|----------|----------|----------|-----------|
| `publishCurrentState()` | [eventserver.cpp#L145](app/eventserver.cpp#L145) | reused `_colorDoc` → `color_event` | `sendRoot()` (TCP) |
| `publishClockSlaveStatus()` | [eventserver.cpp#L194](app/eventserver.cpp#L194) | `JsonRpcMessage` (`clock_slave_status`) | `sendToClients()` |
| `publishKeepAlive()` | [eventserver.cpp#L210](app/eventserver.cpp#L210) | `JsonRpcMessage` (`keep_alive`) | `sendToClients()` |
| `publishTransitionFinished()` | [eventserver.cpp#L226](app/eventserver.cpp#L226) | `JsonRpcMessage` (`transition_finished`) | `sendToClients()` |

### MQTT command / envelope paths — [app/mqtt.cpp](app/mqtt.cpp)

| Function | Location | Document | Notes |
|----------|----------|----------|-------|
| `publishCommand()` | [mqtt.cpp#L394](app/mqtt.cpp#L394) | `JsonRpcMessage` | wraps a caller `JsonObject params` |
| `handleChannelCommand()` | [mqtt.cpp#L809](app/mqtt.cpp#L809) | `DynamicJsonDocument(256)` | forwards the **`JsonObject`** to `api->dispatchCommand()` ([mqtt.cpp#L906](app/mqtt.cpp#L906)); the serialized string is debug-only |

### WiFi status broadcast — [app/networking.cpp](app/networking.cpp)

| Function | Location | Document | Transport |
|----------|----------|----------|-----------|
| `broadcastWifiStatus()` | [networking.cpp#L476](app/networking.cpp#L476) | `JsonRpcMessage` (`wifi_status`) | WebSocket broadcast |

### mDNS / cluster updates — [app/mdnsHandler.cpp](app/mdnsHandler.cpp)

| Function | Location | Document | Transport |
|----------|----------|----------|-----------|
| `sendWsUpdate()` | [mdnsHandler.cpp#L516](app/mdnsHandler.cpp#L516) | `serializeJsonPretty(host, ...)` | `app.wsBroadcast()` (WebSocket) |

Serializes a caller-supplied `JsonObject host` built by the controllers layer.

### WebSocket initial OTA push — [app/webserver.cpp](app/webserver.cpp)

| Function | Location | Document | Transport |
|----------|----------|----------|-----------|
| `wsConnected()` | [webserver.cpp#L213](app/webserver.cpp#L213) | `DynamicJsonDocument(256)` → `JsonRpcMessage` (merges `params`) | `socket.sendString()` |

---

## Excluded (built but not transmitted)

These construct ArduinoJson documents that are persisted to flash, not sent:

- `ApplicationOTA::saveStatus()` — [otaupdate.cpp#L626](app/otaupdate.cpp#L626)
  (`StaticJsonDocument<128>` → `Json::saveToFile`).
- Config load/save paths using `deserializeJson` / `Json::loadFromFile` are
  inbound and out of scope.
