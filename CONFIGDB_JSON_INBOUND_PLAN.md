# ConfigDB Inbound JSON Migration Plan

## Status

Outbound JSON generation is **done**: every outbound site across HTTP, WebSocket,
TCP event server and MQTT now builds its payload via `rpcCodec()` /
`Jsonrpc::Root` (ConfigDB-generated types from
[jsonrpc.cfgdb](jsonrpc.cfgdb) + [params.cfgdb](params.cfgdb) +
[value-types.cfgdb](value-types.cfgdb)) and serializes through
`RpcCodec::render()` / `renderPayload()` or `ConfigDB::Object::createExportStream()`.
See [JSON_OUTBOUND_USAGE.md](JSON_OUTBOUND_USAGE.md) for the full inventory and
migration status of that half of the work.

This document covers the other half: **inbound** JSON — everything the firmware
currently *parses* with ArduinoJson from an HTTP request body, a WebSocket
frame, or an MQTT payload.

### Relationship to CONFIGDB_JSON_MIGRATION_PLAN.md

[CONFIGDB_JSON_MIGRATION_PLAN.md](CONFIGDB_JSON_MIGRATION_PLAN.md) is an
earlier, broader architecture document. It sketches a heavier design — a
dedicated `JsonRpc::*` library, a bounded workspace pool, generated
request/response unions modeled after `../ConfigDB/samples/JsonRpc`, and a
companion frontend code-generation programme. Parts of it (the schema
layering, the transient-store `onCommit`/`clearDirty()` requirement, the
non-owning buffer-adapter idea) are exactly what outbound migration actually
used and what this plan reuses.

This document deliberately scopes down to what's achievable incrementally,
function-by-function, the same way outbound was actually done (see git log:
`a01135f`, `ce39171`, `81ce2a9`) rather than adopting the workspace-pool/library
design up front. Sming here runs a single-threaded cooperative scheduler and
every inbound message is handled synchronously start-to-finish within one
callback — there is no concurrent in-flight request problem today beyond what
`RpcCodec`'s existing re-entrancy guard already covers. Revisit the heavier
design only if/when that stops being true (e.g. genuinely chunked/async
ingress is implemented).

---

## Step 1 — Inventory of current inbound JSON parsing

All of the following use ArduinoJson (`deserializeJson`, `Json::deserialize`,
`JsonObject`/`JsonDocument`) today. "Shape" is a rough complexity signal for
sequencing in Step 3.

### HTTP (`app/webserver.cpp`, POST bodies via `parseJsonBody()`)

| Handler | Route | Reads | Shape |
|---|---|---|---|
| `parseJsonBody()` | helper used by all POST handlers below | `deserializeJson()` from body stream or buffered `String` into a caller-supplied `JsonDocument` | infra, not a payload |
| `onColorPost()` | `POST /color` | `hsv`/`raw` (+ optional `from`), `cmd`, `t`, `r`, `d`, `name`, `channels` | nested, optional variants |
| `onConnect()` | `POST /connect` | `ssid`, `password` | 2 scalars |
| `onUpdate()` | `POST /update` | `rom.url` (optional `spiffs.url`) | 1 nested object |
| `onStop()` / `onSkip()` / `onPause()` / `onContinue()` | `POST /stop,/skip,/pause,/continue` | optional `channels` array | tiny/array |
| `onBlink()` | `POST /blink` | `hsv`/`raw`, ramp time | nested |
| `onToggle()` | `POST /toggle` | none (empty body OK) | empty |
| `onSetOn()` / `onSetOff()` | `POST /on,/off` | `hsv`/`raw` (+ optional `from`), `channels`, ramp | nested, optional variants |
| `onSystemReq()` | `POST /system` | `cmd`, `enable`, `clearOTA` | 3 scalars |

### WebSocket (`app/webserver.cpp` `wsMessage()`)

One entry point parses the full JSON-RPC 2.0 envelope (`jsonrpc`/`method`/`id`/`params`)
with `Json::deserialize()`, then routes `params` (a live `JsonObject`) to either
`Api::dispatchCommand()` or `Api::renderData()` depending on method. This is the
single highest-value/highest-risk site: one parse feeds every WS-originated
command and query.

### MQTT (`app/mqtt.cpp`)

| Handler | Trigger | Reads | Shape |
|---|---|---|---|
| `onMessageReceived()` | any subscribed topic | routes by topic to JSON-RPC sync (`dispatchJsonRpc`), color sync (`dispatchCommand("color", String)`), or HA handlers below | dispatch only |
| `handleChannelCommand()` | HA per-channel `.../<channel>/set` | `state`, `brightness` | 2 scalars |
| `handleHomeAssistantCommand()` | HA main light `.../light/set` | `state`, `brightness`, `color`, `color_temp`, `transition`; **re-encodes** into our own `hsv`/`cmd`/`t` shape and re-dispatches | nested, **foreign schema** (Home Assistant's, not ours) |

### Shared dispatch chain (`app/apihandler.cpp`, `app/jsonrpcmessage.cpp`, `app/jsonprocessor.cpp`)

- `Api::dispatchCommand(const String&, const JsonObject&, ...)` — the real
  dispatcher; switches on method name, calls one of `app.jsonproc.on*()`.
- `Api::dispatchCommand(const char*, const JsonObject&, ...)` — thin overload.
- `Api::dispatchCommand(const String& method, const String& params, ...)` —
  parses `params` with a throwaway `DynamicJsonDocument(512)`, then calls the
  `JsonObject` overload. Used by MQTT's color-sync path.
- `Api::dispatchJsonRpc(const String& json, ...)` — wraps a full JSON-RPC
  string in `JsonRpcMessageIn`, then dispatches.
- `JsonRpcMessageIn` ([jsonrpcmessage.h](include/jsonrpcmessage.h)) — owns a
  `DynamicJsonDocument(MAX_JSON_MESSAGE_LENGTH)`, exposes `getMethod()`/`getParams()`.
- `JsonProcessor` ([jsonprocessor.h](include/jsonprocessor.h)) — one
  `on<Method>(JsonObject root, ...)` per command (`onColor`, `onStop`, `onSkip`,
  `onPause`, `onContinue`, `onBlink`, `onSetOn`, `onSetOff`, `onToggle`,
  `onDirect`), each reading fields out of a live `JsonObject` into a
  `RequestParameters` struct, then validating with `RequestParameters::checkParams()`
  and acting on `app.rgbwwctrl`/`app.network`/etc. This is where almost all of
  the real per-field validation logic lives today.

**Total: ~20 inbound parse sites**, all funneling through `Api::dispatchCommand()`
→ `JsonProcessor::on*()` for anything that mutates state.

---

## Step 2 — Target architecture

### What ConfigDB actually provides (verified against `/opt/sming/Sming/Libraries/ConfigDB`)

- `ConfigDB::Object::importFromStream(const Format&, Stream&)` and
  `ConfigDB::Database::importFromStream(const Format&, Stream&)` — synchronous,
  schema-validating import of a complete JSON document/fragment into a
  store/object (`ConfigDB/Object.h`, `ConfigDB/Database.h`).
- `ConfigDB::Json::Format` implements both; also offers
  `createImportStream()` for chunked/async ingestion (HTTP body streams,
  `Network/HttpImportResource.h`).
- Import is **schema-driven**: each JSON property is matched against the
  `.cfgdb`-generated property table. Type/range/enum mismatches surface via
  `Database::handleFormatError(FormatError, const Object&, const String&)`
  (default: log and continue; override to stop on first error).
- Import supports **partial updates**: a JSON object only needs to carry the
  fields it wants to change; omitted fields keep whatever value the
  target object currently holds. Arrays additionally support selector syntax
  (`x[0]`, `x[]`, `x[0:2]`, delete-by-key) — see
  [README.rst](/opt/sming/Sming/Libraries/ConfigDB/README.rst) for the
  persistent-config use case this was designed for.
- The callback mechanism the user pointed at is the same `onCommit()` /
  `Updater` callback already used for outbound
  (`Jsonrpc::Root::onCommit(_db, [](Jsonrpc::RootUpdater root) { root.clearDirty(); });`
  in [rpccodec.cpp](app/rpccodec.cpp)) — it fires right before a store would be
  committed, receives a typed `Updater`, and can mutate/validate/cancel the
  write. There is no *separate* "inbound" callback API beyond this; import and
  the commit callback are the same mechanism we're already using.

### The one real mismatch to design around: partial update vs. one-shot command

ConfigDB's partial-update model was built for *persistent config* ("send only
what changed, the rest stays as it was"). Our inbound commands are **one-shot,
stateless requests** sharing one transient store (same `rpcCodec()`-style
object, reused across every message). If we import directly into that shared
object, a field omitted in request N would silently retain whatever value
request N-1 left behind — wrong for something like `POST /color` with no `t`
expecting the schema default (`0`), not "whatever the last caller sent".

**Design rule:** every inbound import must start from a clean/default-valued
object, not a dirty leftover one. Two ways to get this, to be confirmed against
generated code before Phase A:
1. Reset the relevant sub-object's fields to schema defaults immediately before
   each `importFromStream()` call, or
2. Construct a fresh `Updater` scope per request if the generated API supports
   discarding prior state when a new updater is opened (needs verification).

This must be settled and verified with a small spike before any handler is
migrated — it's the one place a naive port would introduce a real behavioral
bug (stale-field leakage between unrelated requests).

### Centralization: two existing chokepoints do most of the work

Before planning a handler-by-handler port, it's worth noting the dispatch
chain already centralizes almost everything that matters:

- **`JsonProcessor::parseRequestParams(JsonObject root, RequestParameters&)`**
  ([jsonprocessor.cpp](app/jsonprocessor.cpp)) is the only place that reads
  `hsv`/`raw` (+`from`)/`t`/`s`/`r`/`d`/`name`/`cmd`/`q`/`channels` out of a
  `JsonObject`. `onStop()`, `onSkip()`, `onPause()`, `onContinue()`,
  `onBlink()`, `onDirect()`, and `onSingleColorCommand()` (used by `onColor()`)
  all just call it and then act on the populated `RequestParameters` — none of
  them touch `JsonObject` fields themselves. Migrating this one function to
  read from a generated ConfigDB accessor instead of `JsonObject` migrates
  essentially the entire command surface (7-8 methods) in one change.
- **The `(const String& json, ...)` overloads are pure duplicated boilerplate.**
  Every command method has a twin that exists only so MQTT/relay callers can
  pass a raw string instead of a `JsonObject` (deserialize into a throwaway
  `DynamicJsonDocument`, then call the `JsonObject` overload). This identical
  pattern repeats 9 times. A single shared "import JSON text into an object"
  helper replaces all 9, or they can be deleted once their only remaining
  caller goes through the object-accepting overload directly.
- **`apihandler.cpp`'s `CommandMethodId`/`getCommandMethodId()`** (anonymous
  namespace, top of the file) already centralizes "method name -> which
  `JsonProcessor::on*` to call" for HTTP, WS and MQTT alike. It's the natural
  place to also carry "method name -> which `params.cfgdb` schema member to
  import into" — extend the existing table, don't build a parallel one.
- **The genuinely new piece belongs in `Api`, not in every HTTP handler.**
  HTTP handlers currently each call `parseJsonBody()` (build a `JsonObject`)
  then `dispatchCommand(method, JsonObject, ...)` — ~12 near-identical bodies
  in [webserver.cpp](app/webserver.cpp). One new entry point, e.g.
  `Api::dispatchCommandFromStream(method, Stream& body, errorMsg, relay)`,
  importing directly from `request.getBodyStream()`, turns every HTTP POST
  handler into a one-liner. This is the one piece that's actually new code
  rather than a refactor of existing logic.
- **WS and MQTT need zero call-site changes for any of the above** — they
  already funnel through the shared `JsonObject`-based `dispatchCommand()`/
  `dispatchJsonRpc()`. Their own envelope parsing migrates independently in
  Phase C/D below, decoupled from this centralization.

**New risk surfaced by this:** `parseRequestParams()` reads every `hsv`/`raw`
component through `parseAbsOrRelValue()`, which accepts a bare number, a
relative `+N`/`-N` string, or a percentage string for what is schema-wise a
single field. ConfigDB's JSON-Schema-driven import validates one fixed type
per property, so this polymorphic shape doesn't map directly onto it. Likely
resolution: type these fields as `string-value` in the schema (always accept
the raw token) and keep `parseAbsOrRelValue()`'s existing string-based parser
completely unchanged, trading away ConfigDB's native numeric range validation
for just these fields. Needs its own decision, same as the stale-field-leakage
risk above — both belong in the Phase A spike.

### Proposed mapping, transport by transport

```text
HTTP POST body stream --------> object.importFromStream(Json::format, *bodyStream)
WebSocket text frame ---------> wrap in non-owning Stream adapter -> importFromStream
MQTT payload buffer -----------> wrap in non-owning Stream adapter -> importFromStream
                                              |
                                              v
                                   generated Updater (typed accessors)
                                              |
                                              v
                                   same Api::dispatchCommand() / JsonProcessor
                                   entry points, reading generated getters
                                   instead of JsonObject[] lookups
                                              |
                                              v
                                   existing outbound render path (unchanged)
```

- **Non-owning buffer adapter.** WebSocket and MQTT already hand us a complete,
  assembled message (a `const String&` / payload buffer) — there's no
  `Stream` to read from directly. [CONFIGDB_JSON_MIGRATION_PLAN.md](CONFIGDB_JSON_MIGRATION_PLAN.md)
  already designed exactly this (`BufferInputStream`, a non-owning
  `IDataSourceStream` wrapping `data`/`length`); reuse that design verbatim —
  no need to invent a second one. It must be consumed synchronously before the
  owning callback returns (same constraint already true of `JsonRpcMessageIn`
  today).
- **Schema reuse.** The `raw`/`hsv`/`command-fields` defs in
  [params.cfgdb](params.cfgdb) already model the *outbound* color/command
  shape. The inbound color/command payloads (`onColorPost`, `onBlink`,
  `onSetOn/Off`, MQTT color sync) are the same shape — reuse the same `$defs`
  for import rather than defining parallel inbound-only schema. This needs
  confirming the field-name/casing match exactly (they should, since both are
  "the same command JSON" from two directions).
- **New schema needed.** The WebSocket JSON-RPC *request* envelope
  (`jsonrpc`/`id`/`method`/`params`) has no inbound-shaped schema member today —
  [jsonrpc.cfgdb](jsonrpc.cfgdb) only defines outbound *bodies*. Phase C below
  adds one, following the same payload/framing split already used outbound.
- **Validation/error reporting.** Override `Database::handleFormatError()` (or
  inspect the `Status` returned by `importFromStream()`) and translate
  `FormatError` codes into the existing `errorMsg` conventions
  (`"missing cmd"`, `"malformed json"`, etc.) so API error responses don't
  regress. `RequestParameters::checkParams()`'s *cross-field* validation
  (checks spanning more than one property, e.g. "raw XOR hsv must be set") has
  no ConfigDB equivalent and stays as hand-written post-import validation.
- **Size limits.** `JsonRpcMessageIn` today bounds input to
  `MAX_JSON_MESSAGE_LENGTH` and reports a distinct "too large" error.
  ConfigDB's JSON import parser uses an internal bounded streaming parser —
  confirm its effective size behavior (buffer-per-token vs. whole-message) and
  make sure oversized input still fails with a clear, existing-style error
  rather than a silent truncation.
- **Re-entrancy.** Import and render both read/write the same shared
  transient `Jsonrpc` store behind `rpcCodec()`. `RpcCodec::render()`/`renderPayload()`
  already guard against re-entrant calls (see
  [configdb-conventions.md](/memories/repo/configdb-conventions.md)); add a
  symmetric guard around the new import path, sharing one "busy" flag/scope
  with render/renderPayload rather than inventing a parallel mechanism — a
  single request is always import → dispatch → render → done, never
  interleaved with another message, so one shared store and one shared guard
  is sufficient; no workspace pool needed at current concurrency.
- **Home Assistant MQTT payloads are a declared exception.** `handleHomeAssistantCommand()`
  parses *Home Assistant's* wire schema, not ours — it's foreign and out of
  our `.cfgdb` control. Plan: keep a minimal ArduinoJson parse step just for
  the HA fields, then feed our own command shape through the same
  ConfigDB-import path as everything else, rather than trying to model HA's
  schema in `.cfgdb` too. Full ArduinoJson removal here is not a goal.

---

## Step 3 — Phased rollout

Sequenced to centralize first — migrate the two chokepoints once, then let
that coverage cascade, rather than porting 20 call sites one at a time.

1. **Phase A — spike, then centralize `parseRequestParams()` + the dispatch table.**
   - Spike: verify the partial-update/stale-field question above against the
     actual generated code, and settle the `AbsOrRelValue`/schema-type
     question, both against a small throwaway test.
   - Migrate `JsonProcessor::parseRequestParams()` to read from a generated
     ConfigDB accessor instead of `JsonObject`. This alone covers `onStop`,
     `onSkip`, `onPause`, `onContinue`, `onBlink`, `onDirect`, and
     `onSingleColorCommand`/`onColor`.
   - Extend `apihandler.cpp`'s `CommandMethodId`/`getCommandMethodId()` table
     with the matching `params.cfgdb` schema member per method.
   - Delete the 9 duplicated `(const String& json, ...)` overloads in
     `JsonProcessor` once callers route through the migrated
     object-accepting overload.

2. **Phase B — new HTTP stream entry point.** Add
   `Api::dispatchCommandFromStream(method, Stream&, errorMsg, relay)` and
   point every POST handler in `webserver.cpp` (`onColorPost`, `onStop`,
   `onSkip`, `onPause`, `onContinue`, `onBlink`, `onToggle`, `onSetOn`,
   `onSetOff`, `onSystemReq`) at it, replacing their individual
   `parseJsonBody()` + `JsonObject` construction. `onConnect()`'s POST branch
   and `onUpdate()`'s `rom.url` body can reuse the same entry point once their
   schema members exist (a sibling of `connect-result`, and a small
   `rom`/`spiffs` def respectively).

3. **Phase C — WebSocket JSON-RPC envelope.** Add the inbound request-envelope
   schema member to [jsonrpc.cfgdb](jsonrpc.cfgdb); replace `wsMessage()`'s
   `Json::deserialize()` + `JsonRpcMessageIn`-equivalent parsing with a single
   `importFromStream()` over the `BufferInputStream` adapter; dispatch off the
   generated union tag. Retire `JsonRpcMessageIn` once `dispatchJsonRpc()` no
   longer needs it.

4. **Phase D — MQTT command/sync + Home Assistant.** `onMessageReceived()`'s
   remaining paths and `handleChannelCommand()`; apply the declared HA
   exception above to `handleHomeAssistantCommand()`.

5. **Phase E — cleanup.** Delete the
   `dispatchCommand(String, String, ...)` parse-then-dispatch overload, and
   `parseJsonBody()`'s `DynamicJsonDocument` form (replace with a
   `Status`-returning ConfigDB-import version) once nothing references them.
   Update [JSON_OUTBOUND_USAGE.md](JSON_OUTBOUND_USAGE.md)'s sibling inbound
   inventory (or merge this document's Step 1 table into it) to reflect
   final state.

---

## Open questions / risks (carry into Phase A spike)

- **Stale-field leakage** (see Step 2) — must be resolved before any handler
  migrates, not discovered after.
- **Error message fidelity** — `FormatError` → existing `errorMsg` string
  mapping needs to cover every message current callers rely on (frontend may
  pattern-match on specific error text).
- **Size-limit parity** with `JsonRpcMessageIn`'s current `MAX_JSON_MESSAGE_LENGTH`
  behavior.
- **HA schema exception** — confirm there's no appetite to also model HA's
  schema in `.cfgdb` (would remove the last ArduinoJson dependency entirely,
  but is low value relative to effort since it's a third-party, not our own,
  contract).
