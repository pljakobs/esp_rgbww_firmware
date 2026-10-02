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

Inbound JSON migration is **complete for firmware-owned HTTP, WebSocket, and
MQTT ingress**. HTTP command bodies use
`Api::dispatchCommandFromStream()`; `/system`, `/connect`, and `/update` import
their dedicated ConfigDB payload types. WebSocket and JSON-RPC MQTT messages
import the generated `ws_request` envelope through `BufferInputStream`, and
Home Assistant main-light and per-channel commands import their consumed foreign
fields through dedicated ConfigDB types. No ArduinoJson deserializer remains on
these request paths.

The remaining `deserializeJson()` call in `app/application.cpp` reads locally
stored webapp metadata; it is not an HTTP, WebSocket, or MQTT request parser.
ArduinoJson remains a build dependency because other non-ingress code still
uses its types. Home Assistant MQTT discovery/basic light compatibility remains
available; surfacing stored transitions, groups, and scenes is a separate
integration feature and is not part of this parsing migration.

The earlier StringPool investigation did **not** establish a confirmed leak.
ConfigDB's pool interns distinct strings for the lifetime of a Store, but the
reported host-suite failures were not isolated to that pool and no pool-specific
growth measurement or minimal reproduction was retained. Treat unbounded growth
as an unverified risk, not a demonstrated regression or a blocker. Inbound
HSV/raw and HA scalar tokens continue to import through ConfigDB; no ArduinoJson
exception was added for them.

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

The inventory below describes the remaining parsing and, for migrated command
routes, the former ArduinoJson path. "Shape" is a rough complexity signal for
sequencing in Step 3.

### HTTP (`app/webserver.cpp`, POST bodies via ConfigDB imports)

| Handler | Route | Reads | Shape |
|---|---|---|---|
| `dispatchBodyCommand()` | helper used by command routes | ConfigDB import from request body stream (buffered fallback where needed) | implemented |
| `importConfigBody()` | helper used by `/system`, `/connect`, `/update` | ConfigDB import from request body stream or buffered `String` | implemented |
| `onColorPost()` | `POST /color` | `hsv`/`raw` (+ optional `from`), `cmd`, `t`, `r`, `d`, `name`, `channels` | ConfigDB migrated |
| `onConnect()` | `POST /connect` | `ssid`, `password` | ConfigDB migrated |
| `onUpdate()` | `POST /update` | `rom.url` (optional `spiffs.url`) | ConfigDB migrated |
| `onStop()` / `onSkip()` / `onPause()` / `onContinue()` | `POST /stop,/skip,/pause,/continue` | optional `channels` array | ConfigDB migrated |
| `onBlink()` | `POST /blink` | `hsv`/`raw`, ramp time | ConfigDB migrated |
| `onToggle()` | `POST /toggle` | none (empty body OK) | ConfigDB migrated |
| `onSetOn()` / `onSetOff()` | `POST /on,/off` | `hsv`/`raw` (+ optional `from`), `channels`, ramp | ConfigDB migrated |
| `onSystemReq()` | `POST /system` | `cmd`, `enable`, `clearOTA` | ConfigDB migrated |

### WebSocket (`app/webserver.cpp` `wsMessage()`)

`wsMessage()` imports the JSON-RPC 2.0 envelope (`jsonrpc`/`method`/`id`/`params`)
through the generated `ws_request` schema using a non-owning `BufferInputStream`.
Typed metadata drives authentication/query handling; command params are exported
from the typed object and passed to the ConfigDB command importer.

### MQTT (`app/mqtt.cpp`)

| Handler | Trigger | Reads | Shape |
|---|---|---|---|
| `onMessageReceived()` | any subscribed topic | routes by topic to ConfigDB JSON-RPC sync, color sync, or HA handlers below | ConfigDB dispatch |
| `handleChannelCommand()` | HA per-channel `.../<channel>/set` | `state`, `brightness` | ConfigDB migrated |
| `handleHomeAssistantCommand()` | HA main light `.../light/set` | `state`, `brightness`, `color`, `color_temp`, `transition`; translates to command-request schema and dispatches through ConfigDB | ConfigDB foreign-schema adapter |

### Shared dispatch chain (`app/apihandler.cpp`, `app/jsonrpcmessage.cpp`, `app/jsonprocessor.cpp`)

- `Api::dispatchCommand(const String&, const JsonObject&, ...)` — the real
  dispatcher; switches on method name, calls one of `app.jsonproc.on*()`.
- `Api::dispatchCommand(const char*, const JsonObject&, ...)` — thin overload.
- `Api::dispatchCommandFromStream()` imports each command into the generated
  `command_request` member and passes copied `RequestParameters` to the shared
  command execution methods.
- `Api::dispatchJsonRpc()` and `Api::parseJsonRpcRequest()` import the full
  envelope into `ws_request`; MQTT JSON-RPC retains the former 512-byte limit.
- `JsonRpcMessageIn` and the raw-string `dispatchCommand()` parser overload have
  been removed.
- `JsonProcessor` ([jsonprocessor.h](include/jsonprocessor.h)) — one
  `on<Method>(JsonObject root, ...)` per command (`onColor`, `onStop`, `onSkip`,
  `onPause`, `onContinue`, `onBlink`, `onSetOn`, `onSetOff`, `onToggle`,
  `onDirect`), each reading fields out of a live `JsonObject` into a
  `RequestParameters` struct, then validating with `RequestParameters::checkParams()`
  and acting on `app.rgbwwctrl`/`app.network`/etc. This is where almost all of
  the real per-field validation logic lives today.

All firmware-owned HTTP, WebSocket, and MQTT ingress now enters ConfigDB-backed
import paths. ArduinoJson parsing that remains is outside those request paths.

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
- **Home Assistant MQTT payloads use a foreign wire schema.** `handleHomeAssistantCommand()`
  currently parses HA fields and re-encodes them into our command shape. During
  Phase D, first try a small dedicated ConfigDB schema for the HA fields we
  consume, then send the translated command through the normal ConfigDB import
  path. Keep ArduinoJson here only if the extra schema/import work proves
  disproportionate or technically blocked; minimizing its use remains the goal.

---

## Step 3 — Phased rollout

All migration phases are complete:

1. **Phase A — command import.** The `command_request` schema keeps inbound
   absolute/relative/percentage color tokens as strings while preserving the
   numeric outbound command schema. Every import selects its union member via
   `toXxx()` to reset fields to schema defaults before reading a new request.
2. **Phase B — HTTP.** Command routes use `dispatchCommandFromStream()`;
   `/system`, `/connect`, and `/update` use their own ConfigDB request members
   through `importConfigBody()`. The ArduinoJson `parseJsonBody()` helper is
   removed.
3. **Phase C — WebSocket.** `ws_request` models the envelope and its consumed
   params. `wsMessage()` parses with `BufferInputStream`; `JsonRpcMessageIn` is
   retired. MQTT JSON-RPC uses the same parser and retains its 512-byte limit.
4. **Phase D — MQTT and Home Assistant.** MQTT color sync, HA channel commands,
   and HA main-light commands all import through ConfigDB. The HA adapter
   translates its consumed fields into the normal `command_request` shape.
5. **Phase E — cleanup and audit.** The raw-string command parser and
   `JsonRpcMessageIn` are removed. A source audit finds no ArduinoJson
   deserializer in HTTP, WebSocket, or MQTT ingress. ArduinoJson remains a
   dependency for uses outside this migration, including parsing locally stored
   webapp metadata.

---

## Phase A spike — resolved (2026-10-01)

Both open questions below were settled by reading the actual ConfigDB library
source (`/opt/sming/Sming/Libraries/ConfigDB`), not by guessing or a
throwaway test:

- **Stale-field leakage: resolved.** `Union::setTag()`
  (`src/include/ConfigDB/Union.h`) is documented and implemented as "set the
  current tag and reset content to object default", and the generated
  `to<Item>(tag)` template (same file) always calls `setTag()` before
  returning the Updater:
  ```cpp
  template <typename Item> Item to(Tag tag) {
      setTag(tag);
      return Item(*parent, typeinfo().getObject(tag), dataRef + propinfo().offset);
  }
  ```
  So every `update.toXxx()` call — the same idiom already used throughout the
  outbound migration — resets that member's storage to schema defaults first.
  **Rule going forward: inbound import call sites must use `.toXxx()`, never
  `.asXxx()`** (the latter doesn't reset; it's for continuing to populate an
  already-selected tag or read-only access).
- **`AbsOrRelValue`/schema-type mismatch: resolved, with a design decision.**
  `WriteStream::setProperty()` (`src/Json/WriteStream.cpp`) passes the raw
  JSON token text straight to `Property::setJsonValue()`
  (`src/Property.cpp`) with no JSON-element-type-vs-schema-type gate for
  scalar properties; `Store::parseString()` (`src/Store.cpp`) for a
  `PropertyType::String` property just interns whatever token text it was
  given. A `string-value`-typed field therefore accepts a bare number, a
  `+N`/`-N` relative token, or a `N%` percentage token equally — exactly what
  `parseAbsOrRelValue()` needs.

  **However:** the existing outbound `raw`/`hsv` defs (used by
  `command-fields`, already relied on by `AppMqttClient::publishCommand()`/
  `publishCurrentRaw()`/`publishCurrentHsv()`) are numeric-typed
  (`raw-value`/`hue-value`/etc.), and changing them to `string-value` would
  break those already-shipped outbound setter calls
  (`raw.setR(raw.r)` with a numeric `raw.r`, etc.). Rather than retyping a
  schema member outbound code already depends on, **added a separate,
  inbound-only schema member**: `command-request-fields` (+ `raw-input`,
  `raw-input-base`, `hsv-input`, `hsv-input-base`, `channel-list` in
  [params.cfgdb](params.cfgdb)), exposed at the jsonrpc root as
  `command_request` ([jsonrpc.cfgdb](jsonrpc.cfgdb)), generating
  `update.toCommandRequestFields()` / `root.asCommandRequestFields()`
  (single-level, same pattern as the existing `command` member — verified in
  `out/ConfigDB/jsonrpc.h`). Its `raw`/`hsv` component getters
  (`getR()`/`getH()`/etc., verified in `out/ConfigDB/params.h`) return
  `String`, exactly matching what `parseAbsOrRelValue()` needs once it's
  changed to take a `String`/generated-accessor input instead of a
  `JsonVariantConst`. `t`/`s`/`d` stay `uint32_t`, `r` stays `bool`,
  `cmd`/`name`/`q` stay `String`, `channels` is a plain string array
  (`ChannelListUpdater`/`ContainedChannelList : StringArrayTemplate`) — all
  confirmed from the generated headers, not assumed.

  This schema addition has been added and the project builds clean on Host
  with it (verified, not just written). The next step is migrating
  `JsonProcessor::parseRequestParams()` itself to populate `RequestParameters`
  from a `CommandRequestFieldsUpdater`/`ContainedCommandRequestFields`
  instead of a `JsonObject` — not yet done.

---

## Phase A/B implementation and StringPool review (2026-10-01)

The Phase A/B command path is implemented as described in the Status section.
During an earlier host-suite run, declining free heap and subsequent
rate-limited or failed requests were observed. The run did not identify the
responsible allocation site, and the observed symptom alone did not establish
a ConfigDB or StringPool regression.

### Historical host-suite symptom (cause undetermined)

- `test_simple_fade` (first color POST in the run) passed.
- Every subsequent test failed, with HTTP responses degrading in this order
  as the run progressed: `200 OK` → `429 TOO_MANY_REQUESTS` (with
  `Retry-After` headers, scaled by how far free heap was below the floor) →
  outright connection failures / status `0`.
- This is the firmware's own built-in low-heap rate limiter
  (`ApplicationWebserver::checkHeap()` / `MINIMUM_HEAP`, `app.checkHeap()`)
  doing exactly what it's designed to do: shed load once free heap drops
  below a floor. Free heap declined during this run, but that alone did not
  identify the allocation source or rule out other causes.

### StringPool behavior (not proof of a runtime leak)

1. `ConfigDB/Pool.h`'s `StringPool` class doc comment, verbatim:
   > "We store all string data in a single buffer... **Strings are appended
   > but never removed.**"
2. `Store::parseString()` (`src/Store.cpp`) only takes this
   append-into-StringPool path for `PropertyType::String` properties
   (`stringPool.findOrAdd(...)`); numeric/bool properties instead overwrite a
   fixed-size struct field in place, with no growth.
3. The Phase A schema design made the `hsv`/`raw` leaf fields string-typed so
  they can carry `AbsOrRelValue`'s `+N`/`-N`/`N%` token forms. `Store::parseString()`
  can intern new values into the long-lived `rpcCodec()` Store; this establishes
  a possible growth mechanism, not that the mechanism caused the observed test
  failure.
4. The earlier hypothesis assumed the color tests generated enough unique
  values to produce meaningful pool growth. Neither the unique-value count nor
  resulting StringPool allocation was measured.

The ConfigDB source confirms that distinct strings can accumulate in a
long-lived Store. That is a possible growth mechanism, but it does not prove
that it caused the host-suite symptom or that the current workload experiences
problematic growth. No StringPool-specific allocation trace or minimal
reproduction was retained. There was no demonstrated StringPool regression;
keep this as an unverified measurement question, not a confirmed leak or a
shipping blocker.

### Rejected workarounds considered during investigation

1. **Constructing a fresh `Jsonrpc` instance per request to reclaim its
   StringPool on destruction.** Not attempted in code, but considered and
   rejected: `Database`'s constructor pattern plus the existing
   `onCommit`/`clearDirty()` persistence-avoidance trick suggested stores may
   be opened/associated with a real file path lazily, so repeated
   construct-destroy per request risked either filesystem I/O per request or
   undermining the one-shared-store assumption the `RpcCodec` re-entrancy
   guard depends on. Not verified safe; not pursued.
2. **Calling `ConfigDB::Store::clear()` from the `onCommit` callback**, gated
   behind a custom `_flushPending` flag and a `flushStore()` helper that
   forced an extra commit cycle after each `render()`/`renderPayload()` call,
   specifically to reclaim the StringPool (`Store::clear()` does call
   `stringPool.clear()`, confirmed in `src/Store.cpp` — this part was
   technically correct). **This was still the wrong fix**: fetching the
   actual upstream docs
   ([mikee47/ConfigDB, `feature/json-rpc` branch, README.rst "Commit
   Callbacks" section](https://github.com/mikee47/ConfigDB/tree/feature/json-rpc))
   shows the *only* documented/supported call inside a commit callback is
   `clearDirty()` — there is no documented pattern for also calling
   `Store::clear()` there, and doing so is not something the commit-callback
   mechanism was designed for. Reverted back to the plain, documented form:
   ```cpp
   Jsonrpc::Root::onCommit(_db, [](Jsonrpc::RootUpdater root) { root.clearDirty(); });
   ```
    `RpcCodec::render()`/`renderPayload()` are back to their pre-spike form
    (no `flushStore()` calls). These experiments do not establish a need to
    clear the pool; the current implementation does not add a pool-clearing
    workaround.

### ArduinoJson leaf-parser workaround (withdrawn)

This was a proposed workaround, not an implemented change. It assumed the
StringPool diagnosis was established; that assumption was not supported. The
current implementation continues to import HSV/raw fields through ConfigDB.

The proposal was to leave low-cardinality fields in ConfigDB and parse `hsv`/
`raw`/`from` with a scoped `StaticJsonDocument`. It was not adopted because the
suspected cause was unverified. The current implementation imports these fields
through ConfigDB. Revisit the proposal only if repeatable measurements isolate
problematic growth to these imports.

---

## Validation And Remaining Risks

- Host and ESP8266 firmware builds pass with the generated ConfigDB schemas and
  all migrated handlers.
- `testnet.sh none` could not be run in this environment: the script requires
  privileged TAP/NAT setup, and `tap0` is absent. Running the Host binary on
  loopback is not a substitute because Sming's TAP backend rejects `lo`.
- Runtime/API behavior, including frontend error-text matching, still needs the
  Host smoke suite on a machine where `testnet.sh` can create its TAP interface.
- StringPool growth remains an unmeasured risk described above, not a confirmed
  leak or a blocker.

On Home Assistant, I'm now leaning to an integration module on the home assistant side (see ~/devel/Lightinator_HA_module) that leverages the json-rpc apis provided via http/mqtt/websocket - but minimal discoverability and basic compatibility should be maintained and possibly extended using the Home Assistant mqtt scheme.
Alternatively, we can look for other, possibly more suitable home assistant integrations
The scope of integration to target would be:
- discoverability
- basic operation as hsv light
    - additionally surfacing the transitions as stored in app-data 
    - additionally surfacing groups as stored in app-data
    - additionally surfacing scenes as stored in app-data