# Plan: Extract API Layer + Enable JSON-RPC over WebSocket

## TL;DR

Extract protocol-agnostic API handler logic from `webserver.cpp` into a new `AppApi` class.
Wire the existing `JsonProcessor::onJsonRpc()` to incoming WebSocket frames.
Add request/response correlation. Update webapp to prefer WebSocket for read calls.

The WebSocket is currently **output-only** — incoming frames are silently dropped.
All the building blocks already exist (`JsonRpcMessageIn`, `onJsonRpc`, `wsResource`).

**Key constraint on `onConfig` / `onData`:** Both use `createExportStream()` which
produces a raw `IDataSourceStream*` piped directly as an HTTP body. This cannot be
embedded in a `JsonObjectStream` as a nested value without buffering the full payload
in RAM first — unsafe on ESP8266 with 6–20 KB free heap.

**However**, WebSocket has a streaming send interface. If we wrap the payload stream in
a `JsonRpcStream` envelope class (see Phase 1, step 1b), the full stream can be forwarded
to the WS transport with zero extra buffering. This reopens `getConfig` and `getData`
as candidates for WS dispatch.

> **MQTT caveat:** MQTT on Sming may buffer the full payload before sending, and brokers
> typically enforce a maximum message size (default 256 B on many configs). `getConfig`
> (2–4 KB) will likely exceed this. MQTT dispatch for `getConfig`/`getData` requires
> explicit broker limit verification — excluded from this plan for now.

---

## Phase 1 — Firmware: Extract `AppApi` class

**Goal:** Decouple business logic from HTTP transport so it can be called from any transport.

### Steps

1. Create `include/appApi.h` and `app/appApi.cpp`

   `AppApi` has **two** dispatch entry points to handle the dual return types:

   ```cpp
   // For methods that return a JSON object result (most methods):
   bool dispatch(const String& method, const JsonObject& params, JsonObject& result);

   // For methods that return a stream result (getConfig, getData):
   IDataSourceStream* dispatchStream(const String& method, const JsonObject& params);
   // Returns nullptr if method is unknown or not a stream method.
   ```

   - Constructor takes `Application&` reference
   - `dispatch()` handles: `getInfo`, `getColor`, `setColor`, `getHosts`, `ping`,
     `systemCmd`, `on`, `off`, `stop`, `skip`, `pause`, `continue`, `blink`, `toggle`,
     `networks`, `scanNetworks`, `connect`
   - `dispatchStream()` handles: `getConfig` (read), `getData` (read)
     — returns a raw `IDataSourceStream*` (the export stream, no envelope)
   - Write mutations (`setConfig`, `setData`) stay HTTP-only — not in either dispatch path
   - Color/animation commands **delegate to existing `app.jsonproc.on*()`** — do not duplicate
   - *No new dependencies — can start in parallel with Phase 2 research*

1b. Create `include/jsonRpcStream.h` and `app/jsonRpcStream.cpp` — `JsonRpcStream` wrapper

   A composable `IDataSourceStream` that wraps any existing stream with a JSON-RPC
   envelope, emitting bytes in three sequential segments without buffering:

   ```
   HEADER segment:  {"jsonrpc":"2.0","id":<id>,"result":
   PAYLOAD segment: <inner IDataSourceStream*>
   FOOTER segment:  }
   ```

   ```cpp
   class JsonRpcStream : public IDataSourceStream {
   public:
       JsonRpcStream(int id, IDataSourceStream* payload);  // response
       JsonRpcStream(const String& method, IDataSourceStream* payload); // broadcast

       size_t readMemoryBlock(char* data, size_t bufSize) override;
       bool isFinished() override;

       // seek() is a no-op — Sming never seeks response streams during HTTP send.
       // WS send is also forward-only. Documented as unsupported.
       bool seek(int len) override { return false; }

   private:
       enum class Segment { Header, Payload, Footer, Done };
       Segment _seg{Segment::Header};
       String _header;
       size_t _headerPos{0};
       IDataSourceStream* _payload;
       bool _footerSent{false};
   };
   ```

   **`seek()` note:** Backward seek is not supportable on a composed stream without
   re-creating the inner stream. Forward-only use (HTTP response, WS send) is the
   only supported mode. This is sufficient for all current use cases.

   **WS streaming API note:** Verify that `WebsocketConnection` in the version of Sming
   in use exposes a stream-based send (not just `sendString()`). If only `sendString()`
   is available, `getConfig`/`getData` over WS must read the full stream into a `String`
   first — at which point the memory advantage disappears and they revert to HTTP-only.
   This must be confirmed before implementing step 3's streaming branch.

2. Refactor `webserver.cpp` `on*()` handlers to thin adapters

   - **JSON object methods** (most handlers):
     ```
     preflightRequest()  →  appApi.dispatch(method, params, result)  →  sendApiResponse()
     ```
   - **Stream methods** (`onConfig` GET, `onData` GET) — **HTTP path unchanged**:
     ```
     preflightRequest()  →  appApi.dispatchStream(method, params)  →  sendDataStream()
     ```
     The stream returned by `dispatchStream()` is a bare export stream (no JSON-RPC
     envelope) — HTTP clients continue to receive bare JSON as today.
   - HTTP parsing/serialization/auth stays in webserver
   - *Depends on step 1*

### Excluded from both dispatch paths (HTTP-only, not in `AppApi`)

| Handler | Reason |
|---|---|
| `onFile`, `onIndex`, `onWebapp`, redirectors | Pure HTTP transport, no business logic |
| `onUpdate` | OTA requires chunked HTTP transfer |
| `onConfig` (POST) / `onData` (POST) | Write mutations — correctness over performance |

### Key files

| File | Role |
|---|---|
| `app/webserver.cpp` | All 23 `on*()` handlers — source of business logic to extract |
| `app/jsonprocessor.cpp` | `onColor`, `onStop`, `onBlink`, `onSkip`, `onPause`, `onContinue`, `onDirect` already extracted — **reuse** |
| `include/webserver.h` | Remove business logic declarations, keep transport declarations |
| `include/appApi.h` (new) | `AppApi` class declaration |
| `app/appApi.cpp` (new) | `AppApi` method implementations |
| `include/jsonRpcStream.h` (new) | `JsonRpcStream` class declaration |
| `app/jsonRpcStream.cpp` (new) | `JsonRpcStream` implementation |

---

## Phase 2 — Firmware: Wire WebSocket input to `AppApi`

**Goal:** Accept incoming JSON-RPC frames on `/ws` and dispatch through `AppApi`.

### Current state

`wsResource` has `setConnectionHandler` and `setDisconnectionHandler` registered.
**`setDataHandler` is absent** — incoming frames are silently dropped.

### Steps

3. Add `wsDataReceived()` handler and register it in `webserver.cpp::init()`

   ```cpp
   // In init():
   wsResource->setDataHandler(
       WebsocketDataDelegate(&ApplicationWebserver::wsDataReceived, this));

   // New method:
   void ApplicationWebserver::wsDataReceived(
       WebsocketConnection& socket, const String& msg, size_t size)
   {
       JsonRpcMessageIn req(msg);
       String method = req.getMethod();
       int id = req.getRoot()[F("id")] | -1;
       JsonObject params = req.getParams();

       // Branch 1: stream methods (getConfig, getData)
       // Only viable if WebsocketConnection::sendStream() is available in this Sming version.
       IDataSourceStream* payloadStream = appApi.dispatchStream(method, params);
       if(payloadStream != nullptr) {
           auto* rpcStream = new JsonRpcStream(id, payloadStream);
           socket.sendStream(rpcStream);  // ← verify API exists before implementing
           return;
       }

       // Branch 2: JSON object methods
       DynamicJsonDocument resultDoc(2048);  // sized for actual method — not always 4096
       JsonObject result = resultDoc.to<JsonObject>();

       JsonObjectStream* stream = new JsonObjectStream(2560);
       JsonObject resp = stream->getRoot();
       resp[F("jsonrpc")] = "2.0";
       resp[F("id")] = id;

       if(appApi.dispatch(method, params, result)) {
           resp[F("result")] = result;
       } else {
           JsonObject err = resp.createNestedObject(F("error"));
           err[F("code")] = -32601;
           err[F("message")] = F("Method not found");
       }
       socket.sendString(stream->toString());
       delete stream;
   }
   ```

   > **Pre-condition:** Confirm `WebsocketConnection::sendStream()` exists in Sming 6.2.0
   > before implementing the stream branch. If absent, `getConfig`/`getData` over WS are
   > deferred to a later release and remain HTTP-only.

   *Depends on step 1 and 1b*

4. Request/response correlation ID

   - `JsonRpcMessageIn::getRoot()[F("id")]` already available
   - Response echoes the same `id`
   - Webapp matches responses to pending promises by `id`
   - *Parallel with step 3*

5. Extend dispatch coverage

   `JsonProcessor::onJsonRpc()` today handles only:
   `color`, `stop`, `blink`, `skip`, `pause`, `continue`, `direct`

   `AppApi::dispatch()` additionally handles:
   `getInfo`, `getHosts`, `ping`, `on`, `off`, `toggle`, `networks`, `scanNetworks`,
   `connect`, `systemCmd`

   `AppApi::dispatchStream()` handles:
   `getConfig` (read), `getData` (read) — subject to WS streaming API availability

   **Not dispatched via WS:** `setConfig`, `setData` — write mutations stay HTTP

### Key files

| File | Change |
|---|---|
| `app/webserver.cpp` | Add `wsDataReceived()`, register data handler in `init()` |
| `include/webserver.h` | Add `wsDataReceived()` declaration, add `AppApi appApi` member |
| `app/jsonrpcmessage.cpp` / `include/jsonrpcmessage.h` | No changes needed |
| `include/appApi.h`, `app/appApi.cpp` | New (from Phase 1) |
| `include/jsonRpcStream.h`, `app/jsonRpcStream.cpp` | New (from Phase 1b) |

---

## Phase 3 — Firmware: Fix immediate `/info?v=2` bugs *(independent)*

**Goal:** Fix current truncation of `/info?v=2` response before the full refactor lands.

6. Three fixes in `onInfo()` in `app/webserver.cpp`:

   a. **Increase `JsonObjectStream` capacity** — default 1024 is too small:
      ```cpp
      auto stream = std::make_unique<JsonObjectStream>(4096);
      ```

   b. **Remove the `= 0` overwrites** that blank out `tcp_pcb_size` and
      `tcp_active_estimated_bytes` immediately after their real values are assigned.

   c. **Fix key typos** — remove trailing `: ` from all TCP state JSON keys:
      ```cpp
      debug[F("established")] = tcpStats.established;
      // same for syn_sent, syn_rcvd, fin_wait_1, fin_wait_2,
      // last_ack, time_wait, close_wait, closing, closed
      ```

   *Independent of all other phases — do this now.*

---

## Phase 4 — Webapp: Prefer WebSocket for API reads

**Goal:** Eliminate redundant HTTP connections by routing read calls through the
existing WebSocket connection. Writes remain HTTP.

### Steps

7. Add `request(method, params)` to `src/services/websocket.js`

   - Generates a unique numeric `id`
   - Sends `{jsonrpc:"2.0", method, id, params}`
   - Returns a `Promise` that resolves when a response frame with matching `id` arrives
   - Rejects after 5s timeout → caller falls back to HTTP
   - Stores pending requests in a `Map<id, {resolve, reject, timer}>`
   - Incoming message handler: if frame has `id` found in map, resolve the promise
   - *Depends on Phase 2 firmware deployed*

8. Update `src/stores/infoDataStore.js`

   - Try `wsService.request("getInfo")` first
   - Fall back to `apiService.getInfo()` if WebSocket not connected or request times out
   - *Depends on step 7*

9. *(Optional)* Update `configDataStore.js` and `appDataStore.js` reads to use WebSocket
   - Only viable after WS streaming API confirmed (Phase 2 step 3 pre-condition)
   - **Writes stay HTTP** — `setConfig`, `setData` mutations
   - *Depends on step 7 + Phase 1 step 1b + WS streaming confirmed*

### Stays HTTP regardless

| Endpoint | Reason |
|---|---|
| `setConfig`, `setData` | Write mutations — correctness over performance |
| `getConfig`, `getData` | Conditional on WS streaming API availability in Sming 6.2.0 |

### Key files

| File | Change |
|---|---|
| `src/services/websocket.js` | Add `request()` + pending-request `Map` |
| `src/stores/infoDataStore.js` | WS-first fetch with HTTP fallback |
| `src/stores/configDataStore.js` | Optional WS-first read (conditional) |
| `src/stores/appDataStore.js` | Optional WS-first read (conditional) |
| `src/stores/colorDataStore.js` | Optional WS-first read |

---

## Verification

1. Flash Phase 3 fix, `GET /info?v=2` — confirm full JSON including
   `rgbww`, `connection`, `network`, `mqtt` objects and all TCP state counters
2. Flash Phase 2 firmware, open browser console — confirm WebSocket receives
   `getInfo` response frame with matching `id`
3. Run `test/collect_info_v2_log.py` — confirm `tcp_connections` stays at 2–3
   (WS + logger) under browser load instead of spiking to 5
4. Confirm `iconsSprite.svg` fetched once only (boot-file fix already applied)
5. Compare `minimumfreeHeapRuntime` before/after — expect improvement due to
   fewer simultaneous TCP PCBs during initial page load

---

## Open questions (must resolve before implementation)

| # | Question | Blocks |
|---|---|---|
| Q1 | Does `WebsocketConnection` in Sming 6.2.0 expose a stream-based send API? | Phase 2 step 3 stream branch; Phase 4 step 9 |
| Q2 | What is the Sming MQTT client's maximum outbound message size? | MQTT dispatch of `getConfig`/`getData` |

---

## Decisions

- **Writes stay HTTP** — `setConfig`, `setData` — correctness over performance for mutations
- **Two dispatch entry points** — `dispatch()` for JSON object results, `dispatchStream()`
  for stream results — required by the incompatible return types
- **HTTP path for `getConfig`/`getData` unchanged** — `dispatchStream()` returns a bare
  export stream; the JSON-RPC envelope is added only for WS, not for HTTP, so existing
  HTTP clients are unaffected
- **`JsonRpcStream::seek()` is a no-op** — forward-only use (HTTP response, WS send)
  is the only supported mode; documented explicitly
- **`getConfig`/`getData` over WS conditional** on confirming `WebsocketConnection::sendStream()`
  exists in Sming 6.2.0 — deferred if absent
- **MQTT streaming excluded** — message size limits make `getConfig`/`getData` unsafe;
  requires explicit broker limit verification before enabling
- **`JsonProcessor::onJsonRpc()` is reused**, not replaced — `AppApi` delegates animation
  commands to it, same as MQTT path today
- **File-serving handlers stay in webserver** — pure transport, no business logic
- **`onUpdate()` stays HTTP-only** — OTA requires chunked transfer
- **Authentication** — remains in HTTP `preflightRequest()`; WebSocket connections
  authenticated at HTTP Upgrade handshake, not per-message
- **TCP legacy clients (port 9090)** — no changes; EventServer path unchanged
- **MQTT path** — no changes; continues to call `JsonProcessor::onJsonRpc()` directly