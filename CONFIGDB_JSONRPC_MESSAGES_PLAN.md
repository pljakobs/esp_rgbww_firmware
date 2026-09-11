# ConfigDB as JSON-RPC Message Codec — Migration Plan

**Scope.** This plan covers a *transient* use of ConfigDB: marshalling outbound
JSON-RPC 2.0 messages. It is **unrelated to the two existing conventional
ConfigDB uses** in the firmware (`app.cfg` → [app-config.cfgdb](app-config.cfgdb)
and `app.data` → [app-data.cfgdb](app-data.cfgdb)), which are persistent,
filesystem-backed configuration stores and are not touched here.

The message database is a *serialization buffer*, not storage: it is populated,
serialized, and discarded. It is never committed to flash.

Companion inventory of the sites to migrate:
[JSON_OUTBOUND_USAGE.md](JSON_OUTBOUND_USAGE.md).

### Schema layering — framing vs payload

The three schema files are a deliberate three-layer stack, and the split is the
central reason this migration is worth doing:

| File | Layer | Role |
|---|---|---|
| [value-types.cfgdb](value-types.cfgdb) | scalars | reusable constrained primitives (`raw-value`, `hue-value`, `error-code`, …) |
| [params.cfgdb](params.cfgdb) | **payload** | transport-independent objects: `color-params`, `info-v1-params`, `info-v2-params`, `netlist-item`, `error`, … |
| [jsonrpc.cfgdb](jsonrpc.cfgdb) | **framing** | wraps a payload in the JSON-RPC 2.0 envelope; each root member's title is the wire method name |

**Consequence:** the payload is exactly what the HTTP webserver interface
sends and receives — bare, unframed. The JSON-RPC envelope applies only to the
WebSocket, TCP event-server and MQTT transports.

So there are two distinct serialization entry points over *the same* populated
payload object:

- **HTTP** → `ConfigDB::Json::ReadStream(store, paramsObject)` (or
  `Object::createExportStream(ConfigDB::Json::format)`) — emits the payload
  object alone.
- **WS / TCP / MQTT** → `JsonRPC::ReadStream(db, msg)` — emits
  `{"jsonrpc":"2.0",…,"params"|"result":<same payload>}`.

A single producer (e.g. `handleInfo()`) therefore populates one
`InfoV2ParamsUpdater` and both transports render it, which removes the current
duplication between the REST handlers and the RPC handlers. Any payload that is
only ever sent over HTTP does **not** need a root member in
[jsonrpc.cfgdb](jsonrpc.cfgdb) at all — it lives purely in
[params.cfgdb](params.cfgdb).

---

## 1. Current state (verified)

- [jsonrpc.cfgdb](jsonrpc.cfgdb), [params.cfgdb](params.cfgdb) and
  [value-types.cfgdb](value-types.cfgdb) **already build**. `out/ConfigDB/jsonrpc.h`
  is generated and contains the `Jsonrpc` database class with a `Root` union
  (`toColor()`, `toInfo()`, `asError()`, `getTagString()`, `RootUpdater`, …).
- **Nothing in `app/` or `include/` includes it yet.** The message-codec work is
  greenfield; no existing behaviour depends on it.
- Generated root union is ≈190 B of binary struct plus a `StringPool`, against
  the 128–768 B `StaticJsonDocument`/`DynamicJsonDocument` instances listed in
  the inventory.
- `JsonRPC::ReadStream`
  (`Sming/Libraries/ConfigDB/src/include/ConfigDB/JsonRPC/ReadStream.h`)
  **is an `IDataSourceStream`**, and
  `WebsocketConnection::send(IDataSourceStream*, WS_FRAME_TEXT)` exists
  (`Sming/Components/Network/src/Network/Http/Websocket/WebsocketConnection.h#L106`).
  So HTTP `sendDataStream()` and unicast WebSocket frames can consume a message
  directly, with no intermediate `String`.
- The envelope is emitted by the library, not by us:
  `{"jsonrpc":"2.0"[,"id":N][,"method":"<root tag>"],"params"|"result"|"error":{…}}`.
  **The method name is the root union tag** — schema titles are the wire method
  names.
- `WebsocketConnection::broadcast()` and `MqttClient::publish()` are
  buffer-only (`const char*`/`String`), so those paths need a rendered string.

### Inventory document is stale

[JSON_OUTBOUND_USAGE.md](JSON_OUTBOUND_USAGE.md)'s 🟡 section describes the
pre-`JsonWriter` code. [app/webserver.cpp](app/webserver.cpp) and
[app/apihandler.cpp](app/apihandler.cpp) have since migrated from
`JsonObjectStream` to `JsonWriter`; `handleColor`/`handleNetworks`/`handleInfo`
now take a `JsonWriter::ObjectScope&`. Re-read those files before acting on that
table.

---

## 2. Blockers to clear before any code is written

### 2.1 ConfigDB version — RESOLVED

`/opt/sming/Sming/Libraries/ConfigDB` was moved from `023d286` to `7a6c4a2`
(head of `upstream/feature/json-rpc`), on local branch `feature/json-rpc-head`.
The previous position is preserved as branch
`backup/pre-jsonrpc-upgrade-023d286`. Done in place: a second copy under
`Components/` would compete with the `/opt/sming` one and Sming resolves that
badly. The firmware builds against the upgraded library.

The API is now:

```cpp
struct Message { int id; Kind kind; String method; explicit operator bool() const; };
bool exportMessage(const Message& msg, const ConfigDB::Object& body, Print& out);
class ReadStream : public IDataSourceStream {
    ReadStream(const Message& msg, const ObjectRef& body, bool pretty = false);
};
Message importMessage(const String& jsonString, WriteStream::Callback& callback);
```

**Two consequences, both verified by reading `JsonRPC/ReadStream.cpp` and
running `samples/RGBWWJson`:**

- `method` is a free-form `String`, *not* derived from the root union tag. The
  schema therefore needs one root member per **body shape**, not one per wire
  method. `color` and `color_event` can share a single `color-params` body.
- `body` is an arbitrary `Object` chosen by the caller. Passing the *inner*
  object instead of the union removes the unwanted `{"info-v2-params":{…}}`
  nesting level that the union would otherwise introduce.

### 2.1b Verified runtime behaviour

From building and running `samples/RGBWWJson` on Host:

- No filesystem is required. `open 'jsonrpc/_root.json' failed` is logged and
  the store falls back to defaults.
- Populating a complete info v2 response reported `heap used: 0 bytes`.
- A union serializes only its selected alternative.
- **Every property of a non-union object is always emitted**, including nulls
  and zeros. `ExportOptions` offers only `useName`, `asObject` and `pretty` —
  there is no sparse/skip-defaults option. Any field that is currently
  *conditional* on the wire must therefore become either always-present or a
  union alternative.
- The SIGSEGV that `023d286` produced when generating an info response is fixed
  in `7a6c4a2`.

### 2.2 `error` root member is mis-modelled

`ReadStream::printHeader()` handles `Kind::error` as
`request.findObject("error")`. The current `error` root member in
[jsonrpc.cfgdb](jsonrpc.cfgdb) has `params`/`result` children, so the body is
never found — which is why the sample emits errors as `Kind::result`, producing
non-conformant `{"result":{"code":…}}`. Fix the schema:

```jsonc
{ "type": "object", "title": "error",
  "properties": { "error": { "$ref": "value-types/$defs/error-msg" } } }
```

and send with `Kind::error`.

### 2.3 The `result`-titled root member is mis-modelled

Because the root tag *is* the method name, the network-scan member must be
titled `networks` with a `result` property holding `scanning`/`available` — not
a member literally titled `result`.

### 2.4 Conformance defects to fix first (not deltas to tolerate)

ConfigDB's envelope is spec-conformant, and it exposes three genuine bugs in the
current implementation. These are **fixed**, not worked around — tracked as
Phase 0 in
[CONFIGDB_JSON_MIGRATION_PLAN.md](CONFIGDB_JSON_MIGRATION_PLAN.md).

- **Notifications must not carry an `id`.** `EventServer::sendToClients()`
  ([app/eventserver.cpp](app/eventserver.cpp)) stamps `_nextId++` on every
  event. A notification is defined by the absence of `id`; with one present the
  message is a request and a conforming peer must answer it. `Kind::notification`
  omits the `id` correctly. Retire `_nextId` on the event path.
- **`id` must be an integer, string or null, echoed unchanged.**
  [app/webserver.cpp](app/webserver.cpp) echoes the client's `id` verbatim via
  `writeRawField`, which will happily reflect `true`, an array or an object.
  Validate at the transport boundary, reject illegal types with `-32600`, and
  standardise the webapp on integers so ConfigDB's integer `id` is sufficient.
- **The WebSocket `keep_alive` method is a kludge.** It duplicates native
  PING/PONG control frames (Sming has `WS_FRAME_PING`/`WS_FRAME_PONG` and
  `WebsocketConnection::setPongHandler()`), it shares a method name with the
  unrelated outbound TCP heartbeat, and it carries an explicit WebSocket
  authentication exemption (`isKeepAlive`). Delete it from the WebSocket RPC
  surface and use control frames; keep an application heartbeat only on the raw
  TCP event server, emitted as a proper notification under a distinct name.

### 2.5 Only one message can be in flight

One `Jsonrpc` database == one store == one `Root` union. `JsonRPC::ReadStream`
holds a `StoreRef` until its last byte is read, and only one updater may exist
at a time. With async HTTP / WebSocket / MQTT this **will** collide (an event
firing while an `/info` response is still draining). Choose per call site:

- **Render-now (default).** `MemoryDataStream mem; JsonRPC::exportMessage(db, msg, mem);`
  then hand `mem` (or its string) to the transport. The store is released
  immediately.
- **Stream-direct.** Only where the message is constructed and handed over
  within a single call and nothing else can touch the DB before it drains.

---

## 3. Proposed structure

Add a single small owner — `Components/RpcCodec` or `app/rpccodec.cpp` — holding
**one file-scope `Jsonrpc` instance**, with these rules baked in:

- Never `commit()`. Always `clearDirty()` after populating, so the store is
  never written to flash. Point the database at a path that does not exist so
  `openStore(0)` simply yields defaults.
- Expose three primitives, matching the framing/payload split:
  - `IDataSourceStream* framed(const Message&)` → `new JsonRPC::ReadStream(db, msg)`,
    for unicast WebSocket frames — full JSON-RPC envelope.
  - `IDataSourceStream* payload(ConfigDB::Object&)` →
    `object.createExportStream(ConfigDB::Json::format)`, for HTTP responses —
    the bare [params.cfgdb](params.cfgdb) object, no envelope.
  - `void render(const Message&, String& out)`, for `publish()` and
    `WebsocketConnection::broadcast()`, which are buffer-only. Sming has no
    `Print`→`String` adapter; either add a ~10-line
    `struct StringPrinter : public Print` or use `MemoryDataStream` +
    `moveString()`.
- One RAII helper per message kind, so the `root.update()` scope, the
  `clearDirty()` and the `Message{}` construction cannot be forgotten or
  mismatched.

---

## 4. Migration tiers

### Tier 1 — pilot set (schema exists, biggest win, lowest risk)

| Site | Root tag | Kind | Delivery |
|---|---|---|---|
| `EventServer::publishCurrentState()` [app/eventserver.cpp](app/eventserver.cpp) | `color` | notification | `render()` into `_txBuffer` (keeps capacity reuse; drops the 512 B `_colorDoc`) |
| `publishTransitionFinished()` (event + MQTT) | `transitionFinished` | notification | `render()` |
| `publishClockSlaveStatus()` | `clockSlaveStatus` | notification | `render()` |
| `publishKeepAlive()` — TCP event server only, post-§2.4 rename | heartbeat | notification | `render()` |
| `broadcastWifiStatus()` [app/networking.cpp](app/networking.cpp) | `wifiStatus` | notification | `render()` |
| `AppMqttClient::publishCurrentRaw()` / `publishCurrentHsv()` [app/mqtt.cpp](app/mqtt.cpp) | `color` | notification | `render()` |

These are precisely the sites the inventory marks ❌ *blocked*, because
`JsonRpcMessage` hands out a random-access `getParams()`. ConfigDB unblocks them:
generated typed setters replace random access entirely.

**Exit criterion:** once these five plus `publishCommand()` are converted,
`JsonRpcMessage` and its `DynamicJsonDocument(512)`
([app/jsonrpcmessage.cpp](app/jsonrpcmessage.cpp)) can be deleted.

### Tier 2 — `/info` and the WebSocket RPC reply

This tier is where the framing/payload split pays off: **one** producer, **two**
renderings.

`handleInfo()` currently writes into a `JsonWriter::ObjectScope&`. Change its
signature to take a `Jsonrpc::InfoV2ParamsUpdater&` and set typed fields. Then:

- `onInfo()` (HTTP, unframed payload) →
  `response.sendDataStream(rpc.payload(infoObject), MIME_JSON)`
- `wsMessage()` (framed RPC reply) →
  `socket.send(rpc.framed({id, Kind::result}), WS_FRAME_TEXT)`

The same applies to `handleColor()` / `handleNetworks()`: the REST endpoints get
the bare `color-params` / `netlist-item[]` payload, the RPC transports get it
wrapped. This removes the current need for handlers to be written against a
transport-specific writer type.

This is also where §2.4 (`id` round-tripping) and §2.5 (single message in
flight) bite hardest. Do it only after Tier 1 is proven on hardware.

### Tier 3 — explicitly out of scope

- `publishHADiscovery()`, `publishChannelConfig()`, `publishHAState()`,
  `publishChannelState()` — these are **not** JSON-RPC; they are plain Home
  Assistant payloads. Wrapping them in the RPC envelope would be wrong, and
  modelling them as bare ConfigDB objects buys little over the
  `JsonWriter(String&)` they can already use. Leave on `JsonWriter`.
- `wsSendBroadcast()` / `wsSendRuntimeInfo()` — already pre-formatted, no JSON
  library involved.
- `sendApiCode()` — not JSON-RPC.
- `mdnsHandler::sendWsUpdate()` — serializes a foreign `JsonObject` built by the
  controllers layer; needs that layer reworked first. Separate effort.

---

## 5. Schema gaps to close in `jsonrpc.cfgdb`

The triplet is substantially out of step with the wire. Verified actual shapes,
and the agreed target:

| Message | Actual wire today | Target |
|---|---|---|
| `color_event` | `{mode, raw:{…}, hsv?:{…}}` | `color-params` union — a colour is **always** raw *or* hsv, never both. The union tag conveys what was sent; `mode` becomes redundant. Receivers accept either alternative and expose the tag. |
| `clock_slave_status` | `{offset, current_interval}` | schema rewritten to match the wire (currently declares `sync`/`master_ip`) |
| `transition_finished` | `{name, requeued}` | schema rewritten to match the wire (currently declares `channel`/`duration`) |
| `wifi_status` | `{message?, station:{connected,ssid,dhcp,ip,netmask,gateway,mac}, ap:{enabled,ssid,ip}}` | a proper `wifi-status` definition. Reusing `connection-info-v2` here is a bug. |
| `keep_alive` | no params | TCP event server only (see §2.4) |

### `info` and `sparse`

`sparse` is **not** "omit unset properties" — it selects a *different, smaller
message*. In [app/apihandler.cpp](app/apihandler.cpp) the v2 builder emits the
`runtime` and `debug` blocks only when `sparse` is false. Both variants are
fully specified, so each is simply its own schema object; ConfigDB needs no
sparse-export feature and `/info` stays in scope.

That gives three `info` body shapes: v1 legacy flat, v2 full, v2 sparse. The
conditional `ota` block needs the same treatment or must become
always-present.

Plus the two corrections from §2.2 and §2.3.

When adding a payload, put it in [params.cfgdb](params.cfgdb) first and only add
a [jsonrpc.cfgdb](jsonrpc.cfgdb) root member if that payload is actually sent
over a framed transport (WS / TCP event server / MQTT). HTTP-only payloads need
no framing entry.

---

## 6. Suggested order of work

1. Pin/upgrade Sming ConfigDB; confirm the `Message` API in use (§2.1).
2. Fix the three conformance defects in §2.4 (notification `id`, `id` type
   validation, WebSocket `keep_alive` → PING/PONG), coordinated with the webapp.
3. Schema fixes: `error` shape, `networks` shape, gap list (§2.2, §2.3, §5).
4. Add the `RpcCodec` owner with `framed()` / `payload()` / `render()` (§3).
5. Pilot: convert `EventServer::publishCurrentState()`; verify wire output byte
   for byte against the current implementation and measure heap.
6. Roll out the rest of Tier 1; delete `JsonRpcMessage`.
7. Re-evaluate Tier 2 with measured numbers in hand.
