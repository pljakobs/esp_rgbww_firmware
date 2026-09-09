# ConfigDB JSON Handling Migration Plan

## Goal

Use ConfigDB-generated types as the single JSON contract for HTTP, WebSocket JSON-RPC, and MQTT. JSON is imported and exported through streams at transport boundaries. Application logic receives generated typed objects rather than `JsonObject`, `JsonDocument`, or serialized JSON strings.

The working reference is `../ConfigDB/samples/JsonRpc`:

- `POST /color` imports a standalone color payload from `HttpRequest::getBodyStream()`.
- `/ws` imports a complete JSON-RPC request envelope into a generated union.
- Dispatch uses the generated union tag.
- HTTP and WebSocket responses use ConfigDB export streams.

## Target Architecture

```text
HTTP body stream -----------------> generated payload updater --+
                                                               |
WebSocket/MQTT JSON-RPC frame ----> generated request union ----+--> typed Api handler
                                                               |
MQTT payload ---------------------> generated payload updater --+

Typed result --> generated response/payload updater --> ConfigDB ExportStream --> transport
```

Transport code owns framing, authentication, CORS, HTTP status codes, MQTT topics, and WebSocket connection state. ConfigDB owns JSON parsing, schema validation, typed storage, and serialization. `Api` owns command/query semantics and controller side effects.

**Sequencing.** The migration runs *outbound first*: Phase 1 replaces JSON-RPC
and JSON payload **generation** with ConfigDB across **all** transports (HTTP,
WebSocket, TCP event server, MQTT) before any inbound parsing is touched.
Generation is the half where ConfigDB export is already truly streaming, the wire
format is fully under firmware control, and there are no borrowed-buffer lifetime
constraints — so it validates the schema on the wire at low risk. Inbound
migration follows in Phases 3–5. Phase 0 first corrects three JSON-RPC
conformance defects that ConfigDB's generated envelope will not reproduce.

## Streaming Model and Transport Limits

The term "streaming" must distinguish JSON processing from network ingress. Sming's current APIs are asymmetric:

| Transport | Incoming API | Outgoing API |
|---|---|---|
| HTTP | `HttpRequest::getBodyStream()` | `HttpResponse::sendDataStream()` |
| WebSocket | Complete assembled `const String&` text message | `WebsocketConnection::send(IDataSourceStream*)` |
| MQTT | Complete `mqtt_message_t` payload buffer | `MqttClient::publish(topic, IDataSourceStream*)` |

HTTP already exposes a received body stream. WebSocket and MQTT expose complete messages after Sming has assembled them. Wrapping those buffers in a `Stream` lets ConfigDB parse incrementally and avoids an ArduinoJson DOM, but it does not remove the transport's complete-message buffer.

The initial migration therefore provides **stream-based JSON processing over buffered network ingress**:

```text
TCP chunks
  -> Sming complete-message assembly
  -> borrowed String or MQTT payload buffer
  -> bounded non-owning input Stream
  -> one ConfigDB import
  -> generated request and params objects
```

This still provides meaningful memory improvements:

- No `DynamicJsonDocument` or method-specific ArduinoJson capacity.
- No second parsed representation of the same JSON.
- No serialization and reparse of the nested `params` object.
- Compact generated storage and typed property access.
- Outgoing responses are serialized directly to the network.

It does not make incoming message size unlimited. Input remains bounded by Sming's WebSocket or MQTT message assembly, available heap, configured protocol limits, and ConfigDB storage required by variable-length strings and arrays.

**Decision (from review):** reassembled TCP fragments may exceed the MTU-limited
payload, but that reassembly is owned by the WebSocket/MQTT layer and is not a
ConfigDB concern. True ingress streaming would require a framing protocol layered
*on top of* WebSocket/MQTT; that is explicitly a later option, not a prerequisite.
The buffered compatibility mode described above is the target for this migration.

Note also that outbound generation has none of these constraints: ConfigDB export
streams are genuinely incremental on every transport that accepts an
`IDataSourceStream`. This asymmetry is the reason outbound work is sequenced
first — see [Phase 1](#phase-1-outbound-generation-all-transports).

### One Import, Two Logical Layers

A JSON-RPC envelope and its `params` object are two logical protocol layers, but they must not become two JSON parsing stages. Import the complete envelope into the generated request union in one pass:

```text
JSON stream
  -> generated request union
   -> jsonrpc
   -> id
   -> method discriminator
   -> generated typed params
```

After import, dispatch uses the generated union tag and handlers read `params` through generated accessors. The library must never extract `params` as JSON text for a second parse.

### Compatibility Input Adapter

Provide a reusable bounded, non-owning stream over callback-owned memory:

```cpp
class BufferInputStream : public IDataSourceStream {
public:
  BufferInputStream(const void* data, size_t length);

  uint16_t readMemoryBlock(char* output, int length) override;
  int available() override;
  bool isFinished() override;
};
```

WebSocket can wrap `message.c_str()` and `message.length()`. MQTT can wrap the publish-content pointer and length without first constructing a `String`. ConfigDB import must complete synchronously before the transport callback returns because the adapter does not own the source buffer.

Use an owning `MemoryDataStream` only when data must outlive the callback or the transport has already transferred ownership. Avoid copying a callback-owned `String` merely to satisfy the ConfigDB stream interface.

### True Ingress Streaming

True network ingress streaming requires lower-level Sming receive hooks rather than only the completed-message callbacks. The desired transport interface is:

```cpp
onMessageBegin(const MessageMetadata& metadata);
onMessageData(const uint8_t* data, size_t length);
onMessageEnd();
onMessageAbort(MessageError error);
```

The JSON-RPC transport allocates a workspace and ConfigDB import stream on `begin`, writes each chunk on `data`, finalizes and dispatches on `end`, and discards the workspace on `abort`.

WebSocket integration must preserve message boundaries across TCP chunks and continuation frames, permit control frames between fragments, reject unexpected binary/text transitions, and enforce a configured maximum message size. State is per WebSocket connection.

MQTT integration must preserve topic, QoS, retain flag, packet/message boundaries, and payload length while chunks arrive. State is per in-flight publish operation. Broker and client maximum-packet limits continue to apply.

True ingress streaming is a later optimization. The ConfigDB codec and typed dispatcher must be identical in buffered compatibility mode and chunked mode so transport improvements do not require application-handler changes.

## JSON-RPC Library Architecture

Implement the shared behavior as a transport-neutral library with thin Sming adapters:

```text
JsonRpc::BufferInputStream
  Non-owning compatibility adapter for current callbacks

JsonRpc::Codec
  ConfigDB request import, response export, validation, and error envelopes

JsonRpc::Dispatcher
  Generated request union tag -> typed application handler

JsonRpc::WorkspacePool
  Bounded transient request/result storage and lifecycle management

JsonRpc::WebSocketTransport
  Authentication gate, message/frame lifecycle, connection state, send ownership

JsonRpc::MqttTransport
  Topic routing, QoS/retain metadata, publish lifecycle, send ownership
```

`Codec` accepts `Stream&` and imports the complete generated envelope. It has no dependency on WebSocket, MQTT, authentication, or application globals. `Dispatcher` receives a successfully imported generated request and invokes typed handlers. Transport adapters map transport events and errors into codec operations.

Output is already truly streamed for WebSocket and MQTT. A generated response object creates a ConfigDB `ExportStream`, and the transport takes ownership:

```cpp
socket.send(response.createExportStream(ConfigDB::Json::format).release(), WS_FRAME_TEXT);
mqtt.publish(topic, response.createExportStream(ConfigDB::Json::format).release(), flags);
```

The library must preserve the workspace until the asynchronous output stream is destroyed or signals completion.

## Design Rules

1. Define each command/query payload once as a reusable schema definition.
2. Plain HTTP and payload MQTT topics import that definition directly.
3. JSON-RPC request definitions reference the same definition under `params`.
4. Import/export streams are used at transport boundaries.
5. Business logic uses generated accessors after import; it does not re-read JSON streams.
6. Protocol messages use RAM-only transient workspaces and must never be committed to flash.
7. A workspace remains owned until every export stream referencing its store has completed.
8. Do not share one mutable request object across concurrent HTTP, MQTT, and WebSocket operations.
9. Preserve existing wire formats, aliases, defaults, error codes, and relay behavior during migration.
10. Remove an ArduinoJson path only after equivalent host and target tests pass.
11. Treat a borrowed callback buffer as valid only until that callback returns.
12. Parse each envelope and its nested `params` exactly once.
13. Keep codec and dispatcher APIs independent of buffered versus chunked transport ingress.
14. Enforce explicit message, string, and array size limits in both compatibility and true-streaming modes.

## Prerequisite: Transient Message Storage

The sample currently demonstrates the generated API with a normal ConfigDB database. Firmware protocol traffic needs a non-persistent storage policy because a normal updater commits a dirty store when its last update reference is released.

Provide one of these ConfigDB facilities before production migration:

- Preferred: a RAM-only database/store backend whose commit does not call filesystem save.
- Acceptable interim option: a dedicated transient database subclass with an explicit no-save policy.

Until ConfigDB provides that generalized transient backend, every API workspace
must clear its dirty state from the generated root `onCommit` callback before
the final updater leaves scope. This prevents protocol parsing and response
construction from committing transient JSON data to the underlying persistent
store:

```cpp
JsonRpcApi::Root::onCommit(database, [](auto root) {
  root.clearDirty();
});
```

The callback must be installed when the workspace database is created and must
remain active for its complete lifetime. Do not rely on `store: false` alone as
a no-persistence guarantee during this transition. Any path which opens an
updater without this safeguard is incomplete, including failed imports and
error-response construction.

Use a bounded workspace pool sized from measured concurrency and memory usage. Each workspace contains one generated API store and has states such as `free`, `importing`, `handling`, and `exporting`. If no workspace is available, HTTP returns `409` or `503`, and MQTT/WebSocket reports a busy error or defers processing.

Incremental import must be transactional. A malformed message, disconnect, timeout, or size violation can occur after valid fields have already been written. Import into transient copy-on-write state and expose no side effects until message finalization and schema validation succeed. On failure, discard the complete workspace; never commit partially imported protocol data to live controller or persistent configuration state.

Do not release a workspace immediately after calling `sendDataStream`, `WebsocketConnection::send(stream)`, or `MqttClient::publish(topic, stream)`. ConfigDB export streams hold store state and may be consumed asynchronously. Release the workspace from stream completion/destruction ownership.

## Schema Work

**Authoritative source (decision from review):** the definitive message schema is
the triplet [jsonrpc.cfgdb](jsonrpc.cfgdb) + [params.cfgdb](params.cfgdb) +
[value-types.cfgdb](value-types.cfgdb). Its layering is the point:

| File | Layer | Role |
|---|---|---|
| [value-types.cfgdb](value-types.cfgdb) | scalars | reusable constrained primitives |
| [params.cfgdb](params.cfgdb) | **payload** | transport-independent objects; this is what the HTTP interface sends and receives |
| [jsonrpc.cfgdb](jsonrpc.cfgdb) | **framing** | JSON-RPC 2.0 envelope around a payload; each root member's title is the wire method name |

`json-rpc-api.cfgdb` and the other legacy `.cfgdb` files may define more protocol
messages, but they do not separate framing from payload. Treat them as a
**reference for missing definitions only**, and port those definitions into the
triplet. Do not extend them. Delete them once the triplet is complete.

The triplet is currently incomplete; completing it is part of Phase 1.

Because payload and framing are separate schemas, HTTP renders a
[params.cfgdb](params.cfgdb) object directly and the framed transports wrap the
same object — one producer, two renderings. A payload sent only over HTTP needs
no [jsonrpc.cfgdb](jsonrpc.cfgdb) root member at all.

Keep reusable payloads in [params.cfgdb](params.cfgdb) `$defs`; do not define
envelope variants only as root properties. This avoids generated nested-type
ambiguity and permits HTTP and RPC to share types.

Complete and verify definitions for:

- Color command and command-list payloads
- Animation controls
- System commands
- Info options and legacy aliases
- Networks and hosts query parameters
- Configuration import/export
- JSON-RPC success and error responses
- Notification semantics when `id` is absent or null

Add schema fixtures for accepted legacy payloads and rejected values. Run dbgen plus JSON schema validation in CI.

## Frontend Contract Generation and Validation

> **Deferred (decision from review).** Everything in this section is a later
> programme phase. For the firmware migration the frontend keeps using the
> protocol exactly as it does today; no frontend change is required by Phases
> 1–7 below, because those phases preserve the wire format. The one exception is
> Phase 0, which fixes three genuine JSON-RPC conformance defects and therefore
> needs a coordinated webapp change.
>
> A second complication to resolve before starting it: the Pinia stores mirror
> [app-data.cfgdb](app-data.cfgdb) and [app-config.cfgdb](app-config.cfgdb), yet
> not every JSON-RPC message is a store update — many are notifications or
> actions. A generated store-facing facade therefore cannot be derived from the
> message schema alone.

The Quasar/Vite frontend in `../esp_rgb_webapp2` should consume generated protocol artifacts rather than reimplement firmware constraints by hand. The existing integration points are:

- `src/services/api.js`: centralized HTTP request, retry, authentication, queueing, and response handling.
- `src/services/websocket.js`: JSON-RPC envelope construction, request IDs, pending-request correlation, authentication, and event dispatch.
- `src/services/schemaValidator.js`: handwritten validation, coercion, defaults, and range clamping copied from cfgdb schemas.
- `src/stores/colorDataStore.js`, `infoDataStore.js`, `configDataStore.js`, and `appDataStore.js`: consumers of untyped transport results.
- `src/services/__tests__`: Vitest unit and controller integration tests.

The generated layer belongs between the transport services and stores:

```text
Vue stores and components
  -> generated typed API facade
     -> generated outbound validator
     -> api.js or websocket.js transport
     -> generated inbound validator
  -> validated typed result
```

Transport services continue to own retries, authentication, connection state, timeouts, and request correlation. Generated code owns payload construction rules, method names, request/response types, and runtime contract validation. Stores must never receive unvalidated network data.

### Canonical Schema Artifacts

Publish browser-consumable schemas as firmware build artifacts instead of copying source cfgdb files into the frontend repository:

```text
firmware cfgdb sources
  -> ConfigDB preprocessing
  -> canonical Draft-07 JSON schemas
     -> JSON-RPC request/response schema
     -> reusable payload schemas
     -> OpenAPI components
  -> frontend generation
```

Use the preprocessed schemas from `out/ConfigDB/schema` because they contain resolved cfgdb preprocessing and represent what generated firmware code actually implements. Give every schema and reusable definition a stable `$id`. Do not generate frontend contracts independently from `api.json`, `openapi.json`, and cfgdb sources unless CI verifies they are equivalent.

Package the following artifacts for each firmware API version:

- `json-rpc-api.cfgdb`: authoritative complete request union and response/error envelopes.
- `json-rpc-api.schema.json`: generated browser/OpenAPI projection of the transient cfgdb schema.
- Reusable standalone payload schemas such as color, info, config, and app data.
- `openapi.json`: HTTP paths, verbs, status codes, authentication, and component references.
- `api-version.json`: semantic API version, schema hash, firmware compatibility range, and generator version.

Embed the same API version and schema hash in firmware `/info`. The frontend can then select a compatible generated adapter or report a clear unsupported-version error rather than failing on an arbitrary field.

### What Can and Cannot Be Generated

JSON Schema directly provides:

- TypeScript payload and envelope types.
- Required and optional properties.
- Raw/HSV and request-envelope unions.
- Numeric and string ranges, enums, constants, aliases, and array item types.
- Runtime validators for incoming and outgoing JSON values.

JSON Schema alone does not define HTTP paths, methods, status codes, authentication, retry policy, or which response type belongs to a JSON-RPC request. Generate those from explicit transport metadata:

- Use OpenAPI, with `$ref` links to the canonical component schemas, for the HTTP client.
- Generate JSON-RPC methods from each request union alternative's `method.const`, `params` reference, and an explicit response reference.
- If response mapping cannot be expressed cleanly in the schema, maintain one small versioned method manifest next to the schema rather than inferring it from names.

Example method metadata:

```json
{
  "color": {
    "request": "#/definitions/colorCommandRequest",
    "params": "#/definitions/colorParams",
    "result": "#/definitions/colorResult"
  },
  "getColor": {
    "request": "#/definitions/getColorRequest",
    "params": "#/definitions/emptyParams",
    "result": "#/definitions/colorResult"
  }
}
```

The generator must fail when two alternatives use the same method constant, a method lacks a params/result mapping, or a referenced definition cannot be resolved.

### Generated Frontend Layout

Keep generated files isolated and never edit them manually:

```text
src/generated/api/
  schema/
    json-rpc-api.cfgdb
    openapi.json
    api-version.json
  types.d.ts
  validators.js
  rpc-methods.js
  rpc-client.js
  http-client.js
  index.js
```

The application currently uses JavaScript with `jsconfig.json`, so generated runtime modules can remain JavaScript while `types.d.ts` supplies editor and JSDoc type checking. A later TypeScript migration can consume the same generated declarations without changing the wire layer.

Recommended development dependencies:

- `json-schema-to-typescript` for Draft-07 TypeScript declarations.
- `ajv` as a build-time compiler for strict runtime validators.
- Ajv standalone code generation so the browser bundle contains generated validator functions rather than the complete Ajv runtime.
- `openapi-typescript` for HTTP operation and component declarations.
- Either a small repository generator or a suitable OpenAPI client generator for the thin HTTP method facade.

Bundle size matters on the ESP8266-hosted webapp. Measure generated validator and client chunks before adopting a runtime-heavy generator. Generate only schemas reachable from actual HTTP/RPC methods and allow infrequently used domains, such as configuration or Home Assistant data, to load validators lazily.

### Runtime Validation Policy

Treat all network input as `unknown` until a generated validator accepts it:

```js
const raw = JSON.parse(event.data);
const envelope = assertRpcResponse(raw);
const pending = pendingRequests.get(envelope.id);
const result = assertRpcResult(pending.method, envelope);
pending.resolve(result);
```

Validate outbound payloads immediately before `JSON.stringify()` or `fetch()`:

```js
export async function setColor(params) {
  assertColorParams(params);
  return rpc.request("color", params);
}
```

Compile Ajv validators with strict server-compatible behavior:

```js
{
  allErrors: true,
  strict: true,
  coerceTypes: false,
  useDefaults: false,
  removeAdditional: false
}
```

Validation and UI normalization are different operations. The current `schemaValidator.js` clamps out-of-range values, floors floats, inserts defaults, and coerces booleans. Keep deliberate UI normalization in clearly named functions such as `normalizeColorInput()`, then run strict generated validation on the normalized result. Do not silently coerce or clamp data received from firmware; an invalid response is a protocol defect and must be surfaced with method, path, schema version, and validator errors.

Legacy compatibility also belongs outside generated validators. Functions such as `normalizeInfoData()` may transform a validated legacy response into the current application model. Define and validate each supported legacy wire shape first, then normalize it. Do not weaken the current schema until both old and new responses happen to pass one permissive validator.

### Generated JSON-RPC Client

Generate a method map which associates each method with its params and result validators:

```js
export const rpcMethods = {
  color: {
    validateParams: validateColorParams,
    validateResult: validateColorResult,
  },
  getColor: {
    validateParams: validateEmptyParams,
    validateResult: validateColorResult,
  },
};
```

`src/services/websocket.js` should retain socket lifecycle and authentication but delegate contract work:

1. `request(method, params)` rejects an unknown generated method.
2. The generated params validator runs before allocating a request ID.
3. The complete response envelope is validated before pending-request lookup.
4. The pending request's generated result validator runs before promise resolution.
5. JSON-RPC error envelopes reject with a structured `RpcError` containing code, message, data, method, and ID.
6. Event subscriptions use a generated event map and validate `message.params` before invoking callbacks.

The generated facade exposes named functions so stores do not pass method strings:

```js
await rpcClient.color(colorParams);
const info = await rpcClient.info({ version: 2, sparse: true });
```

If TypeScript is introduced, generate a method map equivalent to:

```ts
interface RpcMethods {
  color: { params: ColorParams; result: ColorResult };
  info: { params: InfoParams; result: InfoResult };
}

request<M extends keyof RpcMethods>(
  method: M,
  params: RpcMethods[M]["params"],
): Promise<RpcMethods[M]["result"]>;
```

### Generated HTTP Client

Generate HTTP operation wrappers from `openapi.json`, while retaining the existing `_executeApi()` transport implementation for controller selection, authorization, retries, timeout, deduplication, and ESP8266 request serialization.

The generated wrapper should provide endpoint-specific request and response validation:

```js
async function postColor(params, controller) {
  assertColorParams(params);
  const response = await apiService.fetchApi("color", controller, {
    method: "POST",
    body: params,
  });
  return assertColorPostResponse(response);
}
```

Replace broad methods such as `getColorData()` and `postColorData(data)` at store call sites with generated named operations. Keep `fetchApi()` internal as the low-level transport primitive. HTTP status validation must occur before body validation because `204`, JSON success, and JSON error responses have different body contracts.

### Build and CI Integration

Add deterministic scripts to the frontend `package.json`:

```json
{
  "scripts": {
    "api:sync": "node tools/sync-api-schema.mjs",
    "api:generate": "node tools/generate-api.mjs",
    "api:check": "node tools/generate-api.mjs --check",
    "predev": "npm run api:generate",
    "prebuild": "npm run api:generate",
    "pretest": "npm run api:check"
  }
}
```

`api:sync` copies or downloads versioned firmware build artifacts and verifies their hash. `api:generate` writes deterministic output without timestamps or machine-specific paths. `api:check` generates into a temporary directory and fails when committed generated files differ.

Do not hide generation only inside a Vite or Quasar plugin: CI, editors, and tests need the same explicit command. The existing Quasar build hooks can assume generated files are present after `predev` or `prebuild`.

Firmware CI should publish the schema bundle. Frontend CI should:

1. Fetch the exact bundle for its declared firmware API version.
2. Verify schema IDs, version, and hash.
3. Regenerate types, validators, and clients.
4. Fail on uncommitted generated differences.
5. Run ESLint, Vitest, and production bundle-size checks.

### Frontend Contract Tests

Convert handwritten field/range assertions in `schemaValidator.spec.js` into schema fixture tests shared with firmware:

- Every accepted fixture passes ConfigDB import and the generated browser validator.
- Every rejected fixture fails both implementations with the same category.
- Boundary values cover integer versus float, ranges, enums, required fields, additional properties, and union alternatives.
- Legacy fixtures validate against their declared version before normalization.

Add transport-level Vitest coverage:

- Mock `fetch` and verify each generated HTTP operation validates request, status, and response.
- Mock `WebSocket` and verify generated method names, request IDs, params validation, result validation, timeout cleanup, and RPC errors.
- Verify malformed or unknown inbound events never reach Pinia callbacks.
- Verify authentication challenge/retry preserves the original typed method and params.
- Run the existing controller integration suite against firmware built from the same schema hash.

The generated-client migration is complete when `api.js` and `websocket.js` contain transport policy only, stores call named generated operations, `schemaValidator.js` contains only intentional UI normalization/legacy conversion, and no firmware range or required-field rule is duplicated manually in frontend source.

## Phase 0: Protocol Conformance Fixes

Three defects in the current JSON-RPC implementation must be fixed **before**
Phase 1, because ConfigDB's generated envelope is spec-conformant and will not
reproduce them. They are corrected here as bugs in their own right, not worked
around; each requires a coordinated firmware + webapp change and is therefore
the one point in the migration where the wire format deliberately changes.

### 0a. Notifications must not carry an `id`

`EventServer::sendToClients()` ([app/eventserver.cpp](app/eventserver.cpp))
stamps `_nextId++` onto every outbound event. A JSON-RPC notification is defined
by the *absence* of `id`; a message with an `id` is a request and obliges the
peer to respond. Clients that follow the spec must either answer or treat these
as protocol errors.

Fix: emit events with no `id`. `JsonRPC::ReadStream` does this automatically for
`Message::Kind::notification`. Retire `_nextId` for the event path. The webapp
must stop keying anything off the event `id`.

### 0b. Request `id` must be integer or string, and echoed unchanged

[app/webserver.cpp](app/webserver.cpp) round-trips the client's `id` verbatim
via `writeRawField` — which will faithfully echo `true`, an object, or an array,
none of which are legal `id` values. ConfigDB emits an integer.

Fix: validate the inbound `id` at the transport boundary. Accept integer, string
and `null`; reject anything else with `-32600`. Standardise the webapp on
integers so the generated integer-only envelope is sufficient; if string ids must
survive, that is a schema change to make deliberately, not an accident of
`writeRawField`.

### 0c. Replace the WebSocket `keep_alive` kludge

The current arrangement is a kludge in three ways:

- `keep_alive` exists as an application-level JSON-RPC method
  ([app/apihandler.cpp](app/apihandler.cpp)) on a transport that already has
  native PING/PONG control frames. Sming supports both
  (`WS_FRAME_PING`/`WS_FRAME_PONG`, `WebsocketConnection::setPongHandler()`).
- It is simultaneously an *outbound* notification from the TCP event server
  (`publishKeepAlive()`, [app/eventserver.cpp](app/eventserver.cpp)) and an
  *inbound* method from WebSocket clients — one method name, two unrelated
  meanings.
- It is explicitly exempted from WebSocket authentication
  ([app/webserver.cpp](app/webserver.cpp), `isKeepAlive`), which is an
  unauthenticated pre-auth code path.

Fix:

- **WebSocket:** delete the `keep_alive` method and its auth exemption. Use
  PING/PONG control frames, driven by the existing `settings.keepAliveSeconds`
  and a `setPongHandler()` liveness timer. Control frames are handled below the
  RPC layer, so no auth bypass is needed.
- **TCP event server:** keep an application-level heartbeat — a raw TCP stream
  has no control frames — but emit it as a proper notification (no `id`, per
  0a) and give it a distinct method name so it is not confused with the removed
  WebSocket method.

This removes one `CommandMethodId`, one auth special case, and one schema entry
before they are ported into [jsonrpc.cfgdb](jsonrpc.cfgdb).

## Phase 1: Outbound Generation (all transports)

**Do outbound first, and do it everywhere at once.** Message *generation* is
where ConfigDB is unambiguously ready today: export is genuinely streaming on
every transport, the wire format is fully under our control, there is no
borrowed-buffer lifetime problem, and no inbound parsing question has to be
answered first. Inbound migration (Phases 3–5) then lands against a schema that
has already been proven on the wire.

Detailed findings, blockers, per-call-site mapping and ordering for this phase
are in
[CONFIGDB_JSONRPC_MESSAGES_PLAN.md](CONFIGDB_JSONRPC_MESSAGES_PLAN.md).
Summary:

### 1a. Complete the schema triplet

Port every message still defined only in the legacy `.cfgdb` files into
[params.cfgdb](params.cfgdb) (payload) and, where the message is framed, into
[jsonrpc.cfgdb](jsonrpc.cfgdb). Fix the two known modelling defects: the `error`
root member must expose an `error` property (not `params`/`result`), and the
member titled `result` must be titled `networks` with a `result` property.
Reconcile `transition_finished` and `clock_slave_status` field sets against what
the firmware actually sends.

### 1b. Add the transient message workspace

One owner (`Components/RpcCodec` or `app/rpccodec.cpp`) holding the generated
message database, never committed, with three primitives:

- `framed(const Message&)` → `JsonRPC::ReadStream` — full envelope, for unicast
  WebSocket and any stream-capable framed transport.
- `payload(ConfigDB::Object&)` → `Json::ReadStream` — bare payload, for HTTP.
- `render(const Message&, String&)` — for the buffer-only sinks:
  `MqttClient::publish(topic, String)` and `WebsocketConnection::broadcast()`.

See the Prerequisite section above for the no-persistence requirement, and
Design Rule 7 for export-stream lifetime.

### 1c. Convert every outbound site, transport by transport

Using the inventory in [JSON_OUTBOUND_USAGE.md](JSON_OUTBOUND_USAGE.md) — note
that document is stale where it describes `JsonObjectStream`; those sites have
since moved to `JsonWriter`:

| Transport | Sites |
|---|---|
| TCP event server | `publishCurrentState`, `publishTransitionFinished`, `publishClockSlaveStatus`, `publishKeepAlive` [app/eventserver.cpp](app/eventserver.cpp) |
| WebSocket broadcast | `broadcastWifiStatus` [app/networking.cpp](app/networking.cpp), notification/config events [app/webserver.cpp](app/webserver.cpp) |
| WebSocket RPC reply | `wsMessage()` result/error envelope [app/webserver.cpp](app/webserver.cpp) |
| MQTT | `publishCurrentRaw`, `publishCurrentHsv`, `publishTransitionFinished`, `publishCommand` [app/mqtt.cpp](app/mqtt.cpp) |
| HTTP | `onColor`, `onInfo`, `onNetworks`, `onHosts`, … — rendered as **bare payload**, no envelope [app/webserver.cpp](app/webserver.cpp) |

The result producers `handleColor()`, `handleNetworks()` and `handleInfo()`
([app/apihandler.cpp](app/apihandler.cpp)) change signature once, from
`JsonWriter::ObjectScope&` to the generated params updater, and then serve both
the HTTP and the framed renderings.

**Exit criteria for Phase 1:**

- `JsonRpcMessage` and its `DynamicJsonDocument(512)`
  ([app/jsonrpcmessage.cpp](app/jsonrpcmessage.cpp)) are deleted.
- `_colorDoc` and the per-publish `Static`/`DynamicJsonDocument` instances in
  [app/mqtt.cpp](app/mqtt.cpp) and [app/eventserver.cpp](app/eventserver.cpp)
  are gone.
- Every migrated payload is byte-for-byte identical to the pre-migration output,
  verified against captured fixtures — with the Phase 0 conformance fixes
  already applied to the reference fixtures, so no unexplained delta remains.
- No inbound path has changed.

Explicitly **not** in Phase 1: Home Assistant discovery/state/config payloads
and `sendApiCode()`. These are not JSON-RPC and gain little over the
`JsonWriter(String&)` they already use; leave them.

## Phase 2: Typed Color Core

Introduce a typed color entry point in `Api` or the controller-facing command layer:

```cpp
ApiResult handleColor(const ApiSchema::ColorParams& params, bool relay);
```

Move the semantics currently reached through:

```cpp
app.api->dispatchCommand("color", JsonObject, errorMsg, relay);
```

into this typed handler. It must support:

- `hsv` and `raw` alternatives
- Single commands and command lists
- Transition fields (`cmd`, `t`, `s`, `r`, `d`, `name`, `q`, `channels`)
- Existing defaults and validation behavior
- Relay enabled/disabled behavior

Keep the ArduinoJson overload temporarily as an adapter for unmigrated callers. The adapter is removed at the end of Phase 5.

## Phase 3: HTTP `/color`

Retain `bodyToStringParser` initially so Sming owns request buffering and moves the completed `MemoryDataStream` to `request.bodyStream`.

`POST /color` flow:

1. Run `preflightRequest()` for heap, CORS, method, and authentication checks.
2. Require `Content-Type: application/json`.
3. Acquire a transient API workspace.
4. Open the generated standalone color payload updater.
5. Import from `request.getBodyStream()` with `ConfigDB::Json::format`.
6. Map malformed or schema-invalid input to the current API error response.
7. Call the typed color handler.
8. Preserve current relay behavior and response body/status compatibility.

`GET /color` flow:

1. Read live `RGBWWCtrl` state.
2. Populate a generated color result updater.
3. Create a ConfigDB JSON export stream.
4. Pass ownership to `response.sendDataStream()`.

The `GET` half is already done by Phase 1; only the `POST` import path is new here.

After behavior is stable, evaluate direct chunk import through an `HttpResource`. Do not combine `bodyToStringParser` and a direct ConfigDB `ImportStream` on one request because both use `request.args` during body processing.

## Phase 4: WebSocket JSON-RPC

Replace `DynamicJsonDocument` and `JsonRpcMessageIn` for migrated methods.

Input flow:

1. Wrap the complete callback-owned WebSocket `String` in a bounded non-owning input stream.
2. Import the complete generated request envelope.
3. Validate JSON-RPC version, method, required fields, and typed `params` through the schema.
4. Dispatch using the generated request union tag rather than `strcmp()`.
5. Invoke the same typed handler used by HTTP.

The current callback receives a complete assembled text message, so this phase removes the ArduinoJson DOM and extra copies but does not remove Sming's WebSocket message buffer. Keep the import synchronous while the borrowed `String` remains valid. Do not move from or retain the callback-owned `const String&`.

Output flow:

1. Populate the generated response envelope with the request `id`.
2. Populate typed `result` or JSON-RPC `error`.
3. Create a ConfigDB export stream.
4. Call `socket.send(stream.release(), WS_FRAME_TEXT)`.

The output flow is already delivered by Phase 1; this phase only replaces the
input side.

Preserve authentication as a WebSocket transport concern. Authentication must complete before acquiring a command workspace or applying side effects.

Notifications with no `id` execute without sending a response. Parse errors use `-32700`; invalid request, method, params, and internal errors retain the agreed JSON-RPC codes.

After compatibility mode is stable, prototype lower-level WebSocket message begin/data/end callbacks. Feed continuation-frame payload chunks into one ConfigDB import stream held in per-connection state. Preserve ping, pong, and close handling independently of fragmented JSON messages.

## Phase 5: MQTT

Migrate the two MQTT input shapes separately.

### Command topic

The command topic carries a complete JSON-RPC envelope. Import it into the same generated request union used by WebSocket and dispatch through the same RPC dispatcher.

### Color topic

The color topic carries the payload directly. Import it into the same standalone color payload type used by `POST /color`, then call the typed color handler with relay disabled.

For incoming MQTT callbacks, expose `msg->publish.content` as a bounded non-owning input stream and import synchronously before returning. Do not construct a JSON `String` unless ownership must outlive the callback. The MQTT topic may still be copied when required for routing or deferred work, but payload parsing must not require a second contiguous buffer.

For outgoing state and command publications:

1. Populate generated payload or envelope objects.
2. Create a ConfigDB export stream.
3. Use `MqttClient::publish(topic, stream.release(), flags)`.

Delivered by Phase 1; retained here for completeness.

Preserve retain and QoS flags. The publishing workspace must live until the MQTT client releases the stream.

After compatibility mode is stable, evaluate lower-level MQTT publish begin/data/end hooks. A chunked adapter must retain topic, QoS, retain flag, expected payload length, bytes received, workspace, and importer status for each in-flight publish. Abort and discard the workspace on disconnect, timeout, length mismatch, parser rejection, or configured-size overflow.

Home Assistant discovery documents are a separate schema surface. Migrate them after command/state paths because they are large, infrequent, and have different compatibility risk.

## Phase 6: Queries and Configuration

Migrate in increasing order of complexity:

1. `getColor` / color state
2. `info` / `getInfo`
3. `networks` / `getNetworks`
4. `hosts` / `getHosts`
5. `config` / `getConfig`
6. System and animation commands

For dynamic data such as network arrays and runtime info, populate generated response objects only if their bounded storage cost is acceptable. Keep an existing specialized streaming producer where materializing a complete ConfigDB object increases peak memory. ConfigDB should be the default JSON contract, not a reason to regress proven low-memory streams.

Persistent configuration import is distinct from transient API message import. Validate a typed API request first, then deliberately update `AppConfig` in a separate transaction.

## Phase 7: Remove ArduinoJson Protocol Paths

After all transports use generated objects:

- Remove `JsonRpcMessageIn` and its fixed-capacity document.
  (The outbound `JsonRpcMessage` and the per-publish documents were already
  removed by Phase 1.)
- Remove `dispatchCommand(..., JsonObject, ...)` and string-parsing overloads.
- Replace `getCommandMethodId()` and `getDataMethodId()` with generated union dispatch.
- Remove `_colorPostDoc` and endpoint-specific `JsonDocument` buffers.
- Remove duplicate HTTP and MQTT color parsing.
- Retain ArduinoJson only for intentionally unmigrated formats such as third-party Home Assistant documents, then track those separately.

## Error Mapping

Define one transport-independent result type:

```cpp
enum class ApiError {
    None,
    InvalidParams,
    MethodNotFound,
    Busy,
    Unauthorized,
    Internal,
};

struct ApiResult {
    ApiError error;
    String message;
};
```

Map it at transport boundaries:

| API result | HTTP | JSON-RPC |
|---|---:|---:|
| Invalid JSON | 400 | -32700 |
| Invalid request/schema | 400 | -32600 |
| Unknown method | 404/400 | -32601 |
| Invalid params | 400 | -32602 |
| Busy/update conflict | 409/503 | server error |
| Unauthorized | 401 | authentication error |
| Internal failure | 500 | -32603 |

Preserve the firmware's current client-visible payload shape until clients and tests have migrated.

## Validation

For every migrated method, test the same fixtures through HTTP, WebSocket, and MQTT where applicable.

Required tests:

- Valid raw and HSV color payloads
- Boundary values and out-of-range rejection
- Missing required properties
- Unknown properties
- Malformed JSON
- Wrong JSON-RPC version and method
- Request IDs and notifications
- Single color commands and command lists
- Relay true/false behavior
- Concurrent HTTP, WebSocket, and MQTT requests
- Callback-buffer lifetime and non-owning input adapters
- WebSocket messages split across TCP reads and continuation frames
- WebSocket control frames interleaved with fragmented messages
- MQTT payloads split across network reads
- Message abort, disconnect, timeout, and declared-length mismatch
- Identical results in buffered compatibility and chunked ingress modes
- Configured message, string, and array size limits
- Workspace exhaustion
- Partial import rollback with no application side effects
- Export stream lifetime after handler return
- No filesystem writes from protocol workspaces
- Existing authentication, CORS, OTA load shedding, QoS, and retain behavior
- Firmware and frontend schema version/hash agreement
- Deterministic frontend regeneration with no uncommitted differences
- Shared acceptance/rejection fixtures passing ConfigDB and browser validators
- Generated HTTP and JSON-RPC clients rejecting invalid inbound responses

Measure on ESP8266 before and after each phase:

- Free heap before request, at import peak, during handler, and during response
- Minimum free heap under parallel requests
- CONT stack high-water usage
- Allocation count and largest transient allocation
- Firmware/IRAM size
- Request latency

## Completion Criteria

The migration is complete when:

- HTTP payloads and JSON-RPC `params` use the same generated schema definitions.
- WebSocket and MQTT command envelopes use one generated request union and dispatcher.
- Buffered and chunked transports use the same codec and typed dispatcher.
- Application handlers accept generated typed objects only.
- HTTP, WebSocket, and MQTT output uses streams where supported.
- WebSocket and MQTT compatibility adapters do not copy complete callback payloads.
- Protocol workspaces are bounded, concurrency-safe, and non-persistent.
- Failed or interrupted incremental imports cannot modify live state.
- No migrated path parses or serializes the same JSON twice.
- The frontend consumes versioned firmware schema artifacts and generated clients.
- Frontend stores receive only validated generated result types.
- Handwritten frontend code does not duplicate cfgdb constraints.
- Compatibility and target memory tests pass on ESP8266 and ESP32.
