# Firmware Review Items

> **Validation pass: 2026-04-21.** All listed issues verified against current code. Four Application issues (H1/H2/H3/M4) resolved; eleven new issues added. All other issues confirmed present.

## application

> **Resolved:** H1 (null ptr in mount — guard present), H2 (VERSION bounds — clamp+null-term present), H3 (_uptimeMinutes — initialized to 0), M4 (_fs_mounted — all branches set the flag).

### High Severity

1. `logRestart()` dereferences `rtc_info` without null check  
   **Location:** [app/application.cpp](app/application.cpp#L689)  
   **Details:** `logRestart()` accesses `app.rtc_info->reason` without a prior null-check. On cold boots where RTC state was not preserved, `rtc_info` is null and this call crashes the device.
   -> fix

### Medium Severity

1. Restart/AP stop flow race  
   **Location:** [app/application.cpp](app/application.cpp#L750)  
   **Details:** Restart should not proceed immediately while AP shutdown is still pending.
   -> is that restarting the AP or restarting the device? If it's restarting the AP fix, otherwise, thesystem restart will sort things out

2. Delayed switch ROM behavior  
   **Location:** [app/application.cpp](app/application.cpp#L813)  
   **Details:** `switch_rom` command should honor delayed execution semantics consistently with other delayed commands.
   -> fix

3. Clear pin pull-up mode  
   **Location:** [app/application.cpp](app/application.cpp#L474)  
   **Details:** Active-low clear pin should use pull-up to avoid floating reads and spurious triggers.
   -> the clear pin has an external pull up

4. MQTT client ID dead logic  
   **Location:** [app/application.cpp](app/application.cpp#L667)  
   **Details:** Computed `mqttClientId` appears unused in the current init path and should be wired or removed.

### Low Severity

5. Header static version symbol strategy  
   **Location:** [include/application.h](include/application.h#L33)  
   **Details:** Header-level static version symbols duplicate per translation unit; prefer a single-source definition strategy.


6. Missing stopServices implementation  
   **Location:** [include/application.h](include/application.h#L49)  
   **Details:** Declared lifecycle API lacks implementation.
   -> there is no state where the controller would be running but the services would be stopped. The controller is either fully running or shut down or restarting

7. loadbootinfo declaration unused  
   **Location:** [include/application.h](include/application.h#L137)  
   **Details:** Declared method not implemented/invoked.

8. logRestart currently unused  
   **Location:** [include/application.h](include/application.h#L139), [app/application.cpp](app/application.cpp#L689)  
   **Details:** Implemented method appears not integrated into restart path. See also H1 for the null-deref risk within its body.
   -> is this related to the syslog implementation? 

9. Public mutable internals in Application  
   **Location:** [include/application.h](include/application.h#L90)  
   **Details:** Broad public mutable state increases coupling and side-effect risk; encapsulation should be tightened.

## controllers

### High Severity

1. Ping updates are effectively dropped  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L149), [app/controllers.cpp](app/controllers.cpp#L150), [app/controllers.cpp](app/controllers.cpp#L76)  
   **Details:** `updateFromPing()` calls `addOrUpdate(id, "", "", ttl)`, but `addOrUpdate` rejects empty hostname/IP and returns early, so ping-based TTL/state updates never apply.
   -> ping to verify neighbour health has been abandoned an can be removed

2. Unchecked `app.data` dereferences across runtime methods  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L98), [app/controllers.cpp](app/controllers.cpp#L231), [app/controllers.cpp](app/controllers.cpp#L307), [app/controllers.cpp](app/controllers.cpp#L330), [app/controllers.cpp](app/controllers.cpp#L393)  
   **Details:** Constructor checks `app.data`, but many other methods dereference it unconditionally, risking crashes if init order/state changes.
   -> app data is a smart-ptr - isn't it? why would it go away? However. The same is true for app.config, right? 

3. Potential unterminated C strings due to `strncpy` usage  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L185), [app/controllers.cpp](app/controllers.cpp#L200), [app/controllers.cpp](app/controllers.cpp#L337), [app/controllers.cpp](app/controllers.cpp#L338), [app/controllers.cpp](app/controllers.cpp#L398), [app/controllers.cpp](app/controllers.cpp#L399)  
   **Details:** `strncpy` does not guarantee null termination when source length >= destination size; later `strlen`/print can read past bounds.
   -> fix

4. Expiry cleanup condition is unreachable — stale entries accumulate without bound  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L159), [app/controllers.cpp](app/controllers.cpp#L170)  
   **Details:** TTL is clamped with `max(0, ...)` before storage, but `removeExpired()` checks `ttl <= -300`, which can never be true. Stale controller entries are therefore never purged; the list grows permanently over long uptime. *(Upgraded from Medium: the purge mechanism is entirely inoperative.)*
   -> propose a better fix. Controllers that have not been seen for five minutes through mDNS should be marked visible=false

### Medium Severity

1. Non-thread-safe static buffers in getters  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L184), [app/controllers.cpp](app/controllers.cpp#L199)  
   **Details:** `getIpAddress()` and `getHostname()` return pointers to static buffers that are overwritten on subsequent calls.
   -> suggest a better way

2. Hostname protection rule is documented but not enforced  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L116)  
   **Details:** Comment says hostname updates should avoid group/leader names, but code updates hostname whenever it differs.
   -> not sure I understand

3. Iterator still dereferences `app.data` without guard  
   **Location:** [include/controllers.h](include/controllers.h#L56), [app/controllers.cpp](app/controllers.cpp#L317)  
   **Details:** Iterator initialization uses `*app.data` directly and can crash even if constructor guarded earlier.
   -> app.data is the handle of a ConfigDB object and is a smart-ptr, why would this ever fail?

4. Iterator `operator*` creates a fresh ConfigDB view on every dereference  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L317)  
   **Details:** Each `operator*` call constructs a new `AppData::Root::Controllers(*app.data)`. If the underlying ConfigDB is modified between dereferences during a traversal, the iterator observes inconsistent state mid-loop.
   -> this is unintended, what is a better way?
   | The iterator already stores configControllers as a member (constructed once in the constructor). The fix is to use that stored view in operator* instead of constructing a new AppData::Root::Controllers(*app.data). Concretely: store an internal AppData::Root::Controllers iterator position alongside 
   | currentIndex so operator* just dereferences the cached position rather than re-scanning from index 0. The iterator's operator++ advances both currentIndex and the internal iterator. This reduces operator* from O(n) scan to O(1) access.
### Low Severity

5. JSON printer repeatedly re-opens and scans ConfigDB per chunk  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L491), [app/controllers.cpp](app/controllers.cpp#L513)  
   **Details:** Streaming loop performs repeated DB traversal, increasing overhead.

6. Unconditional debug logging in filter path can flood logs  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L486)  
   **Details:** `debug_i` inside `shouldIncludeController` executes for every entry and can hurt performance/noise levels.

# EventServer Review Items

## High Severity

1. `enabled` is never initialized  
   **Location:** [include/eventserver.h](include/eventserver.h#L53), [include/eventserver.h](include/eventserver.h#L37)  
   **Details:** `bool enabled;` has no default value, so `isEnabled()` may read indeterminate state.

2. Keep-alive timer is not stopped on shutdown/destruction  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L52), [app/eventserver.cpp](app/eventserver.cpp#L60), [app/eventserver.cpp](app/eventserver.cpp#L27)  
   **Details:** `start()` starts `_keepAliveTimer`, but `stop()` only calls `shutdown()` and never stops the timer. Destructor calls `stop()`, so timer callbacks may still run against a stopped/destroying object.

3. Event deduplication uses pointer identity, not HSV value equality  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L111), [app/eventserver.cpp](app/eventserver.cpp#L119)  
   **Details:** `(pHsv == _lastpHsv)` compares addresses, not color values. If the same `HSVCT` object mutates, changes can be incorrectly dropped as "No change."

## Medium Severity

4. `enabled` flag is not enforced in `start()`, `stop()`, or publish paths  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L42), [app/eventserver.cpp](app/eventserver.cpp#L60), [app/eventserver.cpp](app/eventserver.cpp#L108), [include/eventserver.h](include/eventserver.h#L38)  
   **Details:** API exposes `setEnabled/isEnabled`, but core behavior ignores it, making the flag misleading and easy to misuse.

5. `publishCurrentState` keeps stale `_last*` state when events are throttled  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L114), [app/eventserver.cpp](app/eventserver.cpp#L118)  
   **Details:** On throttled return, `_lastRaw/_lastpHsv` are not updated. This can lead to redundant sends later and inconsistent dedupe behavior under rapid changes.

6. `sendToClients` assumes all entries in `connections` are valid `TcpClient*`  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L217), [app/eventserver.cpp](app/eventserver.cpp#L218)  
   **Details:** `reinterpret_cast<TcpClient*>` with no null/liveness check is fragile if container semantics change or stale entries remain.

## Low Severity

7. `webServer` member is set but not actually used for broadcasting  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L44), [app/eventserver.cpp](app/eventserver.cpp#L222), [include/eventserver.h](include/eventserver.h#L62)  
   **Details:** Broadcast uses global `app.wsBroadcast(...)` instead of `webServer->...`; member pointer is effectively redundant.

8. Minor API/style issues reduce clarity  
   **Location:** [include/eventserver.h](include/eventserver.h#L33), [include/eventserver.h](include/eventserver.h#L41), [app/eventserver.cpp](app/eventserver.cpp#L89)  
   **Details:** Uses `NULL` instead of `nullptr`, typo `succesfull`, and leftover comments like "Add this line"/"referal"/"cludge".

# FileMap Review Items

## High Severity

No high-severity runtime or memory-safety defects found in [app/fileMap.cpp](app/fileMap.cpp) and [include/fileMap.h](include/fileMap.h).

## Medium Severity

1. Hard build-time coupling to generated `fileList.h` without explicit guard  
   **Location:** [app/fileMap.cpp](app/fileMap.cpp#L8)  
   **Details:** Compilation depends on generated header presence. If frontend generation is skipped/out-of-order, build fails with poor diagnostics.

2. Hard-coded webapp source path in import macro  
   **Location:** [app/fileMap.cpp](app/fileMap.cpp#L16)  
   **Details:** `PROJECT_DIR "/webapp/" file` assumes fixed layout and can silently break when packaging/build layout changes.

3. Full asset embedding can increase flash usage without visibility controls  
   **Location:** [app/fileMap.cpp](app/fileMap.cpp#L16), [app/fileMap.cpp](app/fileMap.cpp#L21)  
   **Details:** All entries from `FILE_LIST` are imported into firmware image unconditionally; no filtering/profile mechanism is visible here.

## Low Severity

4. Macro-heavy pattern is concise but hard to debug and review  
   **Location:** [app/fileMap.cpp](app/fileMap.cpp#L11), [app/fileMap.cpp](app/fileMap.cpp#L16), [app/fileMap.cpp](app/fileMap.cpp#L21)  
   **Details:** `XX` macro reuse obscures expansion during troubleshooting and makes symbol tracing harder for new contributors.

5. Missing local documentation for generation contract  
   **Location:** [app/fileMap.cpp](app/fileMap.cpp#L7), [include/fileMap.h](include/fileMap.h#L10)  
   **Details:** File references generated inputs but does not specify who generates `fileList.h`, when it runs, or expected schema of `FILE_LIST`.

6. Header relies on macro declaration contract without compile-time validation  
   **Location:** [include/fileMap.h](include/fileMap.h#L10)  
   **Details:** `DECLARE_FSTR_MAP(...)` assumes matching definition exists; no static assertions/check points for map population completeness.

# JsonProcessor / JsonRpcMessage Review Items

## Scope Note

This section fully reviews [include/jsonrpcmessage.h](include/jsonrpcmessage.h) and [app/jsonrpcmessage.cpp](app/jsonrpcmessage.cpp).  
For [include/jsonprocessor.h](include/jsonprocessor.h), findings are interface-level only because implementation behavior lives outside this header.

## High Severity

No high-severity defects found.

## Medium Severity

1. JSON parse result is ignored, so malformed or oversized input is not rejected explicitly  
   **Location:** [app/jsonrpcmessage.cpp](app/jsonrpcmessage.cpp#L61), [app/jsonrpcmessage.cpp](app/jsonrpcmessage.cpp#L63)  
   **Details:** `Json::deserialize(...)` return status is not checked. Downstream calls then operate on `_doc` without distinguishing valid input from parse failure.

2. Fixed input document capacity is hard-coded in constructor instead of using shared limit constant  
   **Location:** [app/jsonrpcmessage.cpp](app/jsonrpcmessage.cpp#L61), [include/jsonrpcmessage.h](include/jsonrpcmessage.h#L23)  
   **Details:** `_doc(1024)` duplicates `MAX_JSON_MESSAGE_LENGTH` (1024), creating drift risk if one value changes later.

3. Incoming JSON-RPC accessor methods do not validate required fields before use  
   **Location:** [app/jsonrpcmessage.cpp](app/jsonrpcmessage.cpp#L66), [app/jsonrpcmessage.cpp](app/jsonrpcmessage.cpp#L76)  
   **Details:** `getParams()` and `getMethod()` assume params/method exist with expected type. Missing validation can silently degrade to empty/default values and make command handling ambiguous.

4. Heap-backed `DynamicJsonDocument` in per-message path may increase fragmentation under frequent command traffic  
   **Location:** [include/jsonrpcmessage.h](include/jsonrpcmessage.h#L47), [app/jsonrpcmessage.cpp](app/jsonrpcmessage.cpp#L61)  
   **Details:** Embedded targets can be sensitive to repeated dynamic allocations. If message rate is high, consider static/preallocated parsing strategy.

## Low Severity

5. Public parsing/building APIs are mostly non-const, which weakens interface guarantees  
   **Location:** [include/jsonrpcmessage.h](include/jsonrpcmessage.h#L27), [include/jsonrpcmessage.h](include/jsonrpcmessage.h#L29), [include/jsonrpcmessage.h](include/jsonrpcmessage.h#L43), [include/jsonrpcmessage.h](include/jsonrpcmessage.h#L44)  
   **Details:** Methods like `getRoot`/`getParams`/`getMethod` could be `const` in several cases, improving contract clarity.

6. String literal in `createNestedObject` uses RAM literal instead of flash macro style used elsewhere  
   **Location:** [app/jsonrpcmessage.cpp](app/jsonrpcmessage.cpp#L41)  
   **Details:** Minor memory-style inconsistency compared to `F("jsonrpc")`, `F("method")`, `F("id")`.

7. Header-level include appears broader than needed and increases compile coupling  
   **Location:** [include/jsonrpcmessage.h](include/jsonrpcmessage.h#L20)  
   **Details:** Including `RGBWWLed/RGBWWLed.h` in this header may be unnecessary for declared types and can slow builds.

8. `JsonProcessor` interface exposes loosely constrained request state and mixed naming consistency  
   **Location:** [include/jsonprocessor.h](include/jsonprocessor.h#L35), [include/jsonprocessor.h](include/jsonprocessor.h#L67), [include/jsonprocessor.h](include/jsonprocessor.h#L80)  
   **Details:** `checkParams` returns `int` (non-self-describing status), and parameter naming differs across overloads (`root`/`json`), which is a maintainability issue.

# LedCtrl Review Items

## High Severity

1. Potential division-by-zero in clock publish cadence  
   **Location:** [app/ledctrl.cpp](app/ledctrl.cpp#L320), [include/ledctrl.h](include/ledctrl.h#L68)  
   **Details:** `setClockMaster(..., interval = 0)` allows `clockMasterInterval` to be zero, but `updateLed()` evaluates `_stepCounter % clockMasterInterval`, which is undefined behavior and can crash/reset MCU.

## Medium Severity

2. Sentinel/interval type mismatch can break timing logic  
   **Location:** [include/ledctrl.h](include/ledctrl.h#L103), [app/ledctrl.cpp](app/ledctrl.cpp#L362), [app/ledctrl.cpp](app/ledctrl.cpp#L377)  
   **Details:** `colorMasterInterval` and `transFinInterval` are `uint32_t`, but logic uses signed-style checks (`>= 0`) which are always true for unsigned types. Disabled sentinel values like `-1` are unreliable with unsigned storage.

3. Re-init leaks `StepSync` object  
   **Location:** [app/ledctrl.cpp](app/ledctrl.cpp#L68)  
   **Details:** `init()` always allocates `new StepSync()` without deleting prior `_stepSync`. If `init()` is called more than once (reinit/recovery path), heap memory leaks.

4. Pin validation can return false too early for SoC pin sets  
   **Location:** [app/ledctrl.cpp](app/ledctrl.cpp#L227)  
   **Details:** In `isPinValid()`, once a matching SoC entry is found, the function returns `false` immediately if the pin is not in that first matching entry. If config contains multiple entries for the same SoC, valid pins in later entries are never checked.

5. Member state fields are not default-initialized in header  
   **Location:** [include/ledctrl.h](include/ledctrl.h#L100)  
   **Details:** `clockMaster`, `colorMaster`, `startupColorLast`, and several intervals have no in-class defaults. Current behavior relies on `init()->reconfigure()` ordering; using methods before full init can read indeterminate state.

6. `abs()` overflow risk in stable-color step calculation  
   **Location:** [app/ledctrl.cpp](app/ledctrl.cpp#L409)  
   **Details:** `abs(_saveAfterStableColorMs - (_numStableColorSteps * RGBWW_MINTIMEDIFF))` — both operands are unsigned-derived; the subtraction can wrap to a very large value before `abs()` sees it, producing an incorrect large result.

## Low Severity

7. Logging call likely missing format specifier  
   **Location:** [app/ledctrl.cpp](app/ledctrl.cpp#L478)  
   **Details:** `debug_i("_timerInterval", _timerInterval);` passes an extra argument without a `%` token, so interval value is likely not logged correctly.

8. API parameter is unused in RAW mode path  
   **Location:** [app/ledctrl.cpp](app/ledctrl.cpp#L540), [app/ledctrl.cpp](app/ledctrl.cpp#L572)  
   **Details:** `direction` is accepted by `setOn`/`setOff` but ignored for `fadeRAW(...)`, which can confuse callers expecting parity with HSV path behavior.

# mdnsHandler Review Items

## High Severity

1. Potential out-of-bounds read from non-terminated buffer in hostname sanitization path  
   **Location:** [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L74), [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L885), [include/mdnsHandler.h](include/mdnsHandler.h#L51)  
   **Details:** Both call sites copy untrusted input using `strncpy(..., sizeof(buf))` without forcing `buf[sizeof(buf)-1] = '\0'` before calling sanitizer. Sanitizer loop tests `*read_ptr != '\0'` and can read past buffer end if no terminator is present.

2. Discovery schema mismatch: swarm parser expects `type=host`, swarm advertiser never publishes `type`  
   **Location:** [include/mdnsHandler.h](include/mdnsHandler.h#L218), [include/mdnsHandler.h](include/mdnsHandler.h#L274), [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L251), [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L304), [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L312)  
   **Details:** The swarm TXT record only includes `id`/`isLeader`/`groups`. Parser requires `type=host` to add controllers, so host discovery is silently dropped for all swarm peers.

3. Handler lifecycle leak risk: class registers itself in mDNS server but destructor does not unregister self  
   **Location:** [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L163), [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L42)  
   **Details:** `mDNS::server.addHandler(*this)` is called, but destructor removes only responder handlers, not `*this`. If object is destroyed/recreated, stale handler pointer risk exists.

## Medium Severity

4. Adaptive interval is computed but not applied to active timer interval  
   **Location:** [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L180), [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L182), [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L463)  
   **Details:** `_currentMdnsTimerInterval` is changed in `onMessage`, but `_mdnsSearchTimer.setIntervalMs(...)` is only done during start. Restarting timer with `startOnce()` alone keeps the old interval.

5. Variable-length stack array from network-derived name length can cause portability/stack risk  
   **Location:** [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L233)  
   **Details:** `char hostname[hostname_len + 1];` is a VLA (non-standard C++ extension) and size comes from packet content; large names can exhaust the stack.

6. Internal hostname state is set but not used for filtering decisions  
   **Location:** [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L111), [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L152), [app/mdnsHandler.cpp](app/mdnsHandler.cpp#L216)  
   **Details:** `searchName` is assigned and logged but routing relies on hardcoded suffix checks; this is maintainability debt and a source of divergence bugs.

## Low Severity

7. Dead/private API declarations remain without active implementation path  
   **Location:** [include/mdnsHandler.h](include/mdnsHandler.h#L361), [include/mdnsHandler.h](include/mdnsHandler.h#L362)  
   **Details:** `pingController` and `pingCallback` are declared but implementations are commented out in source, increasing confusion and drift risk.

8. Comment/config mismatch in ping interval  
   **Location:** [include/mdnsHandler.h](include/mdnsHandler.h#L360)  
   **Details:** Value is `10000` (10 s) while comment says "every minute".

# MQTT Review Items

## High Severity

1. MQTT credentials are written to logs in plain text  
   **Location:** [app/mqtt.cpp](app/mqtt.cpp#L79), [app/mqtt.cpp](app/mqtt.cpp#L81)  
   **Details:** The full URL including username and password is logged via `debug_i`. This can leak broker credentials through serial logs, remote log pipelines, or crash dumps.

2. Reconnect can continue after intentional stop due to missing run-state guard  
   **Location:** [app/mqtt.cpp](app/mqtt.cpp#L35), [app/mqtt.cpp](app/mqtt.cpp#L47), [app/mqtt.cpp](app/mqtt.cpp#L167), [include/mqtt.h](include/mqtt.h#L80)  
   **Details:** `onComplete` always schedules `connectDelayed`, and `stop()` only deletes `mqtt`. There is no active state gate or timer cancel in `stop()`, so reconnect attempts may still be scheduled unexpectedly.

## Medium Severity

3. Transition-finished payload field has a typo likely breaking consumers  
   **Location:** [app/mqtt.cpp](app/mqtt.cpp#L378)  
   **Details:** Field is published as `requequed` instead of `requeued`. Subscribers expecting the correct key will miss state.

4. URL assembly does not escape credentials  
   **Location:** [app/mqtt.cpp](app/mqtt.cpp#L79)  
   **Details:** Username/password are concatenated directly into `mqtt://user:pass@host:port`. Reserved characters such as `@`, `:`, `/` can corrupt URL parsing and cause connection failures.

5. Declared state/methods are not implemented or used, creating lifecycle drift  
   **Location:** [include/mqtt.h](include/mqtt.h#L69), [include/mqtt.h](include/mqtt.h#L80)  
   **Details:** `processHACommand` is declared but not implemented in source, and `_running` is declared but not used to control start/stop/connect behavior.

6. Disconnect callback ignores completion status signal  
   **Location:** [app/mqtt.cpp](app/mqtt.cpp#L35)  
   **Details:** `success` parameter is unused; callback behavior is identical for all outcomes, reducing observability and making failure-mode handling coarse.

7. `publish()` dereferences `mqtt` pointer without null-check  
   **Location:** [app/mqtt.cpp](app/mqtt.cpp#L235)  
   **Details:** `publish()` calls `mqtt->getConnectionState()` without first checking for null. While `isRunning()` guards the outer API, internal publish paths may bypass it and crash if `mqtt` was deleted by `stop()`.

## Low Severity

8. Logging and comments are inconsistent with behavior in a few places  
   **Location:** [app/mqtt.cpp](app/mqtt.cpp#L55), [app/mqtt.cpp](app/mqtt.cpp#L99)  
   **Details:** Some messages/comments suggest connection-state assumptions that are not enforced by code paths, increasing maintenance friction.

# Networking Review Items

## High Severity

1. Wi-Fi password is logged in clear text during connect  
   **Location:** [app/networking.cpp](app/networking.cpp#L229)  
   **Details:** The log line prints both SSID and password. This is a credential leak risk via serial logs, remote logs, and crash dumps.

2. Captive DNS lifecycle is incomplete and can stay active after AP stop  
   **Location:** [app/networking.cpp](app/networking.cpp#L441)  
   **Details:** `dnsServer.start()` is called in `startAp`, but there is no matching `dnsServer.stop()` anywhere in the codebase. This leaves stale DNS behavior and resource leakage when AP is cycled.

## Medium Severity

3. Retry threshold check uses equality, which can miss failure transition  
   **Location:** [app/networking.cpp](app/networking.cpp#L253)  
   **Details:** The condition checks `_con_ctr == DEFAULT_CONNECTION_RETRIES` instead of `>=`. If callback timing or multiple disconnect reasons skip the exact value, the error branch can be bypassed.

4. AP DNS startup allocates heap timer per call with fragile cleanup path  
   **Location:** [app/networking.cpp](app/networking.cpp#L437)  
   **Details:** A `new Timer` is allocated each time `startAp` runs. It is only deleted when AP IP becomes non-zero. If the `apIP.toString() != "0.0.0.0"` condition is never satisfied (AP IP assignment failure), `delete dnsStartTimer` is never called, the timer re-fires indefinitely, and both timer memory and scheduling time leak permanently.

5. Scan state can get stuck true if scan start fails immediately  
   **Location:** [app/networking.cpp](app/networking.cpp#L39), [app/networking.cpp](app/networking.cpp#L87), [include/networking.h](include/networking.h)  
   **Details:** `_scanning` is set before `startScan` and only cleared in `scanCompleted`. If `startScan` fails synchronously, `isScanning()` reports `true` indefinitely.

## Low Severity

6. Header exposes placeholder API that always returns empty data  
   **Location:** [include/networking.h](include/networking.h#L57)  
   **Details:** `getLedGroups()` currently returns an empty vector regardless of state. This can mislead callers and hide unimplemented behavior.

7. Accessors return by value and use inconsistent naming/style  
   **Location:** [include/networking.h](include/networking.h#L44)  
   **Details:** Minor maintainability issue: copies on every call and mixed naming conventions (`get_con_status`, `get_con_err_msg` vs camelCase) in public API.

# OTA Update Review Items

## High Severity

1. Partition index helper conflates not-found with first entry, causing silent failures  
   **Location:** [app/otaupdate.cpp](app/otaupdate.cpp#L628), [app/otaupdate.cpp](app/otaupdate.cpp#L650)  
   **Details:** `getPartitionIndex()` returns `0` when a partition name is not found. `delPartition()` treats index `0` as protected/invalid and returns `false`. This means any operation targeting a missing partition silently fails with no error signal, and any code assuming deletion succeeded will operate on a corrupt state. Two distinct failure modes (not-found and first-partition-protected) collapse onto the same return value.

2. Overlap validation in partition table writer is incorrect and can miss real overlaps  
   **Location:** [app/otaupdate.cpp](app/otaupdate.cpp#L673)  
   **Details:** The check uses `partition.offset + partition.size < partitionEnd`. Correct overlap detection should compare the current entry's start against the previous entry's end. As written, overlapping entries may pass validation and produce a broken partition table.

## Medium Severity

3. OTA status is forced to success on every boot, masking prior states  
   **Location:** [app/otaupdate.cpp](app/otaupdate.cpp#L416)  
   **Details:** `checkAtBoot` always calls `saveStatus(OTA_SUCCESS)`, even when no OTA happened this boot. This reduces diagnostic value and can hide meaningful transitional states.

4. Fixed flash size is hard-coded during partition table rewrite  
   **Location:** [app/otaupdate.cpp](app/otaupdate.cpp#L693)  
   **Details:** `deviceHeader.size` is forced to `0x00400000`. On devices with different flash sizes, table metadata can become inconsistent.

5. String copies into partition entry names may be non-terminated  
   **Location:** [app/otaupdate.cpp](app/otaupdate.cpp#L609), [app/otaupdate.cpp](app/otaupdate.cpp#L636), [app/otaupdate.cpp](app/otaupdate.cpp#L688), [app/otaupdate.cpp](app/otaupdate.cpp#L706)  
   **Details:** `strncpy` with full buffer length does not guarantee trailing null byte. Later `String` conversions/logging can read garbage beyond the name field.

6. Public inline API may call function not implemented on non-ESP8266 builds  
   **Location:** [include/otaupdate.h](include/otaupdate.h#L79), [app/otaupdate.cpp](app/otaupdate.cpp#L595)  
   **Details:** `getSpiffsPartition()` is always available in the header and calls `findSpiffsPartition()`, but the cpp implementation is inside an `ARCH_ESP8266` guard. This is a portability/linkage risk if referenced in other targets.

## Low Severity

7. Conditional compilation style is inconsistent and brittle  
   **Location:** [include/otaupdate.h](include/otaupdate.h#L93)  
   **Details:** Uses `#if ARCH_ESP8266` instead of `#ifdef ARCH_ESP8266` unlike nearby guards. Depending on toolchain defines, this can trigger warnings or unexpected behavior.

8. Typo in method name reduces API clarity  
   **Location:** [include/otaupdate.h](include/otaupdate.h#L73)  
   **Details:** `isProccessing` is misspelled; should be `isProcessing`.

# StepSync Review Items

## High Severity

1. Division-by-zero risk in steering calculation when master clock does not advance  
   **Location:** [app/stepsync.cpp](app/stepsync.cpp#L47)  
   **Details:** `masterDiff` can be 0 (e.g., duplicate/stale master packet), but it is used as divisor in `curSteering` calculation. This produces INF/NaN and destabilizes timer control.

## Medium Severity

2. Overflow-delta helper is off by one on wrap-around  
   **Location:** [include/stepsync.h](include/stepsync.h#L21)  
   **Details:** For wrapped counters, correct delta is `max - prev + cur + 1`. Current code omits `+1`, introducing systematic sync error at wrap boundaries.

3. Unsafely narrowing uint32 deltas into signed int  
   **Location:** [app/stepsync.cpp](app/stepsync.cpp#L39), [app/stepsync.cpp](app/stepsync.cpp#L40)  
   **Details:** `calcOverflowVal` on `uint32_t` values is stored in `int`. Large jumps can overflow signed int and invert control behavior.

4. Catch-up accumulator can overflow over long runtime  
   **Location:** [include/stepsync.h](include/stepsync.h#L28), [app/stepsync.cpp](app/stepsync.cpp#L43)  
   **Details:** `_catchupOffset` is a signed `int` and accumulates indefinitely with no saturation or clamping. Extended drift or long uptime can overflow and corrupt steering decisions.

## Low Severity

5. Mixed float/double arithmetic and implicit truncation in interval scaling  
   **Location:** [include/stepsync.h](include/stepsync.h#L34), [app/stepsync.cpp](app/stepsync.cpp#L47)  
   **Details:** `_steering` is `double`, `curSteering` is `float`, and `nextInt` is `uint32_t`. Repeated implicit conversions can add quantization noise in tight control loops.

6. `debug_i` uses `%d` for `_catchupOffset` which can grow to large values  
   **Location:** [app/stepsync.cpp](app/stepsync.cpp#L44)  
   **Details:** `%d` is correct for `int` but once `_catchupOffset` accumulates past typical ranges (see M4 above), the value is silently truncated in the log. Use `%ld` or add a bound before printing.

# Telemetry Review Items

## High Severity

1. Telemetry credentials are logged in clear text  
   **Location:** [app/telemetry.cpp](app/telemetry.cpp#L84), [app/telemetry.cpp](app/telemetry.cpp#L85)  
   **Details:** URL is built as `mqtt://user:pass@host` and printed directly. This leaks credentials to logs and any remote log sink.

2. Reconnect gate timer callback can outlive object lifecycle  
   **Location:** [app/telemetry.cpp](app/telemetry.cpp#L36), [app/telemetry.cpp](app/telemetry.cpp#L142)  
   **Details:** Timer callback stores and dereferences `this`, but destructor does not stop/disarm the timer. If destruction occurs while timer is armed, callback may dereference a stale pointer.

## Medium Severity

3. MQTT handlers are registered after connect call  
   **Location:** [app/telemetry.cpp](app/telemetry.cpp#L87), [app/telemetry.cpp](app/telemetry.cpp#L88), [app/telemetry.cpp](app/telemetry.cpp#L90)  
   **Details:** `mqtt->connect(...)` is called on L87 before delegates are attached on L88–L90. Fast connect/fail events can occur before handlers are wired, causing missed state transitions and unstable reconnect behavior.

4. Blocking delay in reconnect path can stall the event loop  
   **Location:** [app/telemetry.cpp](app/telemetry.cpp#L106)  
   **Details:** `delay(1000)` blocks cooperative processing and can impact websocket/network responsiveness during reconnect storms.

5. Reconnect gate timestamp type is too narrow and signed  
   **Location:** [include/telemetry.h](include/telemetry.h#L67)  
   **Details:** `millis()` is `unsigned long`, but `_lastReconnectAttempt` is `int`. Long uptime (~24 days) will overflow/sign-wrap and break the 10 s gate logic.

6. Reconnect gating is inconsistent between publish overloads  
   **Location:** [app/telemetry.cpp](app/telemetry.cpp#L127), [app/telemetry.cpp](app/telemetry.cpp#L153)  
   **Details:** The `JsonDocument` `publish()` overload has reconnect gate logic; the `char*` payload overload just returns `false`. `log()` uses the payload-string path, so telemetry logging may never self-recover while stats do.

## Low Severity

7. Header API naming mismatches implementation intent  
   **Location:** [include/telemetry.h](include/telemetry.h#L45)  
   **Details:** `connect` parameters are named `debugServer`/`debugUser`/`debugPass` although the class is telemetry-focused, which is confusing and increases maintenance drift.

# Webserver Review Items

## High Severity

1. Basic auth check is too weak and can accept malformed credentials with password suffix match  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L173)  
   **Details:** After base64 decode, authentication uses `endsWith(configuredPassword)` instead of validating full `user:password` credentials. Any string ending with the configured password passes the check.

2. Potential invalid delete due to uninitialized websocket resource pointer  
   **Location:** [include/webserver.h](include/webserver.h#L46), [app/webserver.cpp](app/webserver.cpp#L73)  
   **Details:** `wsResource` has no in-class initialization and is deleted in destructor. If `init()` was never called, the pointer value is indeterminate and `delete wsResource` is undefined behavior.

3. Wi-Fi credentials are logged in plain text via HTTP connect endpoint  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L1190)  
   **Details:** Password is printed in debug logs (`ssid`/`pass`), leaking secrets into local and remote log sinks.

## Medium Severity

4. Possible out-of-bounds access on empty URI path in static file handler  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L313)  
   **Details:** After substring operations, `fileName[0]` is read without checking `fileName.length() > 0`. If URI is `/`, `fileName` becomes empty and the index access reads the null terminator as if it were content.

5. Response status handling collapses all non-OK codes to 400  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L222)  
   **Details:** `sendApiResponse` ignores the passed status and forces `HTTP_STATUS_BAD_REQUEST` for any non-OK case, reducing protocol correctness.

6. Invalid/odd HTTP header used for auth failure  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L188)  
   **Details:** `response.setHeader(F("401 wrong credentials"), ...)` uses a status code as a header name, which is non-standard and includes spaces. Behavior across clients and proxies is unpredictable.

7. CORS allow-headers omits `Authorization`, breaking secured browser clients  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L1594)  
   **Details:** `setCorsHeaders()` sets `Access-Control-Allow-Headers: Content-Type, Cache-Control` but omits `Authorization`. Browser preflight for authenticated requests will fail.

8. JSON deserialization errors are ignored in multiple command handlers  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L1185), [app/webserver.cpp](app/webserver.cpp#L1259), [app/webserver.cpp](app/webserver.cpp#L1345)  
   **Details:** `Json::deserialize()` return value is not checked in three handlers. Requests proceed with partially parsed/default data, increasing risk of unintended commands.

9. `doc[F("password")].as<const char*>()` returns nullptr when key is absent  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L1190)  
   **Details:** If the POST body is missing the `"password"` key, `as<const char*>()` returns `nullptr`. This value is then passed to `network.startClient(ssid, password)` without a null-guard, which may dereference it internally.

## Low Severity

10. Stray function declaration after unconditional `return` in `onIndex`  
    **Location:** [app/webserver.cpp](app/webserver.cpp#L382)  
    **Details:** After an unconditional `return`, the line `void publishTransitionFinished(const String& name, bool requeued = false);` appears. This is an unreachable, malformed local declaration that some compilers may reject or misparse, and indicates a paste/merge artifact.

11. No heap headroom check between JSON parse and `startClient()` in `onConnect`  
    **Location:** [app/webserver.cpp](app/webserver.cpp#L1184)  
    **Details:** `preflightRequest()` checks heap before entry, but JSON deserialization at L1185 and credential storage consume additional heap before the network call. On a fragmented heap this can silently fail to connect.

# WebSocket Keepalive Review Items# Firmware Review Items

## application

### High Severity

1. Null pointer risk in filesystem fallback mount  
   **Location:** [app/application.cpp](app/application.cpp#L870)  
   **Details:** Secondary partition path can be dereferenced without guaranteeing a valid partition object.
   -> fixed ✅

2. Buffer bounds risk when reading VERSION file  
   **Location:** [app/application.cpp](app/application.cpp#L886)  
   **Details:** `fileRead` result must be validated for negative return and capped before null-termination.
   -> fixed ✅

3. Uptime counter initialization  
   **Location:** [include/application.h](include/application.h#L161)  
   **Details:** `_uptimeMinutes` must be explicitly initialized to avoid undefined startup state.
   -> fixed ✅

### Medium Severity

4. Mounted-state tracking consistency  
   **Location:** [include/application.h](include/application.h#L149), [app/application.cpp](app/application.cpp#L838)  
   **Details:** `_fs_mounted` should be set on all mount success/failure paths and remain authoritative.
   -> fixed ✅

5. Restart/AP stop flow race  
   **Location:** [app/application.cpp](app/application.cpp#L750)  
   **Details:** Restart should not proceed immediately while AP shutdown is still pending.
   -> fixed ✅

6. Delayed switch ROM behavior  
   **Location:** [app/application.cpp](app/application.cpp#L813)  
   **Details:** `switch_rom` command should honor delayed execution semantics consistently with other delayed commands.
   -> fixed ✅

7. Clear pin pull-up mode  
   **Location:** [app/application.cpp](app/application.cpp#L474)  
   **Details:** Active-low clear pin should use pull-up to avoid floating reads and spurious triggers.

8. MQTT client ID dead logic  
   **Location:** [app/application.cpp](app/application.cpp#L667)  
   **Details:** Computed `mqttClientId` appears unused in the current init path and should be wired or removed.

### Low Severity

9. Header static version symbol strategy  
   **Location:** [include/application.h](include/application.h#L33)  
   **Details:** Header-level static version symbols duplicate per translation unit; prefer a single-source definition strategy.

10. Missing stopServices implementation  
    **Location:** [include/application.h](include/application.h#L49)  
    **Details:** Declared lifecycle API lacks implementation.

11. loadbootinfo declaration unused  
    **Location:** [include/application.h](include/application.h#L137)  
    **Details:** Declared method not implemented/invoked.

12. logRestart currently unused  
    **Location:** [include/application.h](include/application.h#L139), [app/application.cpp](app/application.cpp#L689)  
    **Details:** Implemented method appears not integrated into restart path.

13. Public mutable internals in Application  
    **Location:** [include/application.h](include/application.h#L90)  
    **Details:** Broad public mutable state increases coupling and side-effect risk; encapsulation should be tightened.

## controllers

### High Severity

1. Ping updates are effectively dropped  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L149), [app/controllers.cpp](app/controllers.cpp#L150), [app/controllers.cpp](app/controllers.cpp#L76)  
   **Details:** `updateFromPing()` calls `addOrUpdate(id, "", "", ttl)`, but `addOrUpdate` rejects empty hostname/IP and returns early, so ping-based TTL/state updates never apply.

2. Unchecked `app.data` dereferences across runtime methods  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L98), [app/controllers.cpp](app/controllers.cpp#L231), [app/controllers.cpp](app/controllers.cpp#L307), [app/controllers.cpp](app/controllers.cpp#L330), [app/controllers.cpp](app/controllers.cpp#L393)  
   **Details:** Constructor checks `app.data`, but many other methods dereference it unconditionally, risking crashes if init order/state changes.

3. Potential unterminated C strings due to `strncpy` usage  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L185), [app/controllers.cpp](app/controllers.cpp#L200), [app/controllers.cpp](app/controllers.cpp#L337), [app/controllers.cpp](app/controllers.cpp#L338), [app/controllers.cpp](app/controllers.cpp#L398), [app/controllers.cpp](app/controllers.cpp#L399)  
   **Details:** `strncpy` does not guarantee null termination when source length >= destination size; later `strlen`/print can read past bounds.

### Medium Severity

4. Expiry cleanup condition is unreachable  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L159), [app/controllers.cpp](app/controllers.cpp#L170)  
   **Details:** TTL is clamped with `max(0, ...)`, but removal checks `ttl <= -300`, which can never occur, so stale entries are never purged.

5. Non-thread-safe static buffers in getters  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L184), [app/controllers.cpp](app/controllers.cpp#L199)  
   **Details:** `getIpAddress()` and `getHostname()` return pointers to static buffers that are overwritten on subsequent calls.

6. Hostname protection rule is documented but not enforced  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L116)  
   **Details:** Comment says hostname updates should avoid group/leader names, but code updates hostname whenever it differs.

7. Iterator still dereferences `app.data` without guard  
   **Location:** [include/controllers.h](include/controllers.h#L56), [app/controllers.cpp](app/controllers.cpp#L317)  
   **Details:** Iterator initialization uses `*app.data` directly and can crash even if constructor guarded earlier.

### Low Severity

8. JSON printer repeatedly re-opens and scans ConfigDB per chunk  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L491), [app/controllers.cpp](app/controllers.cpp#L513)  
   **Details:** Streaming loop performs repeated DB traversal, increasing overhead.

9. Unconditional debug logging in filter path can flood logs  
   **Location:** [app/controllers.cpp](app/controllers.cpp#L486)  
   **Details:** `debug_i` inside `shouldIncludeController` executes for every entry and can hurt performance/noise levels.

# EventServer Review Items

## High Severity

1. `enabled` is never initialized  
   **Location:** [include/eventserver.h](include/eventserver.h#L53), [include/eventserver.h](include/eventserver.h#L37)  
   **Details:** `bool enabled;` has no default value, so `isEnabled()` may read indeterminate state.

2. Keep-alive timer is not stopped on shutdown/destruction  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L52), [app/eventserver.cpp](app/eventserver.cpp#L60), [app/eventserver.cpp](app/eventserver.cpp#L27)  
   **Details:** `start()` starts `_keepAliveTimer`, but `stop()` only calls `shutdown()` and never stops the timer. Destructor calls `stop()`, so timer callbacks may still run against a stopped/destroying object.

3. Event deduplication uses pointer identity, not HSV value equality  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L111), [app/eventserver.cpp](app/eventserver.cpp#L119)  
   **Details:** `(pHsv == _lastpHsv)` compares addresses, not color values. If the same `HSVCT` object mutates, changes can be incorrectly dropped as "No change."

## Medium Severity

4. `enabled` flag is not enforced in `start()`, `stop()`, or publish paths  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L42), [app/eventserver.cpp](app/eventserver.cpp#L60), [app/eventserver.cpp](app/eventserver.cpp#L108), [include/eventserver.h](include/eventserver.h#L38)  
   **Details:** API exposes `setEnabled/isEnabled`, but core behavior ignores it, making the flag misleading and easy to misuse.

5. `publishCurrentState` keeps stale `_last*` state when events are throttled  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L114), [app/eventserver.cpp](app/eventserver.cpp#L118)  
   **Details:** On throttled return, `_lastRaw/_lastpHsv` are not updated. This can lead to redundant sends later and inconsistent dedupe behavior under rapid changes.

6. `sendToClients` assumes all entries in `connections` are valid `TcpClient*`  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L217), [app/eventserver.cpp](app/eventserver.cpp#L218)  
   **Details:** `reinterpret_cast<TcpClient*>` with no null/liveness check is fragile if container semantics change or stale entries remain.

## Low Severity

7. `webServer` member is set but not actually used for broadcasting  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L44), [app/eventserver.cpp](app/eventserver.cpp#L222), [include/eventserver.h](include/eventserver.h#L62)  
   **Details:** Broadcast uses global `app.wsBroadcast(...)` instead of `webServer->...`; member pointer is effectively redundant.

8. Minor API/style issues reduce clarity  
   **Location:** [include/eventserver.h](include/eventserver.h#L33), [include/eventserver.h](include/eventserver.h#L41), [app/eventserver.cpp](app/eventserver.cpp#L89)  
   **Details:** Uses `NULL` instead of `nullptr`, typo `succesfull`, and leftover comments like "Add this line"/"referal"/"cludge".
# FileMap Review Items

## High Severity

No high-severity runtime or memory-safety defects found in [app/fileMap.cpp](app/fileMap.cpp) and [include/fileMap.h](include/fileMap.h).

## Medium Severity

1. Hard build-time coupling to generated `fileList.h` without explicit guard  
   **Location:** [app/fileMap.cpp](app/fileMap.cpp#L8)  
   **Details:** Compilation depends on generated header presence. If frontend generation is skipped/out-of-order, build fails with poor diagnostics.

2. Hard-coded webapp source path in import macro  
   **Location:** [app/fileMap.cpp](app/fileMap.cpp#L16)  
   **Details:** `PROJECT_DIR "/webapp/" file` assumes fixed layout and can silently break when packaging/build layout changes.

3. Full asset embedding can increase flash usage without visibility controls  
   **Location:** [app/fileMap.cpp](app/fileMap.cpp#L16), [app/fileMap.cpp](app/fileMap.cpp#L21)  
   **Details:** All entries from `FILE_LIST` are imported into firmware image unconditionally; no filtering/profile mechanism is visible here.

## Low Severity

4. Macro-heavy pattern is concise but hard to debug and review  
   **Location:** [app/fileMap.cpp](app/fileMap.cpp#L11), [app/fileMap.cpp](app/fileMap.cpp#L16), [app/fileMap.cpp](app/fileMap.cpp#L21)  
   **Details:** `XX` macro reuse obscures expansion during troubleshooting and makes symbol tracing harder for new contributors.

5. Missing local documentation for generation contract  
   **Location:** [app/fileMap.cpp](app/fileMap.cpp#L7), [include/fileMap.h](include/fileMap.h#L10)  
   **Details:** File references generated inputs but does not specify who generates `fileList.h`, when it runs, or expected schema of `FILE_LIST`.

6. Header relies on macro declaration contract without compile-time validation  
   **Location:** [include/fileMap.h](include/fileMap.h#L10)  
   **Details:** `DECLARE_FSTR_MAP(...)` assumes matching definition exists; no static assertions/check points for map population completeness.

# JsonProcessor / JsonRpcMessage Review Items

## Scope Note

This section fully reviews [include/jsonrpcmessage.h](esp_rgbww_firmware/include/jsonrpcmessage.h) and [app/jsonrpcmessage.cpp](esp_rgbww_firmware/app/jsonrpcmessage.cpp).  
For [include/jsonprocessor.h](esp_rgbww_firmware/include/jsonprocessor.h), findings are interface-level only because implementation behavior lives outside this header.

## High Severity

No clear high-severity defects found in the provided code fragments.

## Medium Severity

1. JSON parse result is ignored, so malformed or oversized input is not rejected explicitly  
   Location: [app/jsonrpcmessage.cpp](esp_rgbww_firmware/app/jsonrpcmessage.cpp#L61), [app/jsonrpcmessage.cpp](esp_rgbww_firmware/app/jsonrpcmessage.cpp#L63)  
   Details: Json::deserialize(...) return status is not checked. Downstream calls then operate on _doc without distinguishing valid input from parse failure.

2. Fixed input document capacity is hard-coded in constructor instead of using shared limit constant  
   Location: [app/jsonrpcmessage.cpp](esp_rgbww_firmware/app/jsonrpcmessage.cpp#L61), [include/jsonrpcmessage.h](esp_rgbww_firmware/include/jsonrpcmessage.h#L23)  
   Details: _doc(1024) duplicates MAX_JSON_MESSAGE_LENGTH (1024), creating drift risk if one value changes later.

3. Incoming JSON-RPC accessor methods do not validate required fields before use  
   Location: [app/jsonrpcmessage.cpp](esp_rgbww_firmware/app/jsonrpcmessage.cpp#L66), [app/jsonrpcmessage.cpp](esp_rgbww_firmware/app/jsonrpcmessage.cpp#L76)  
   Details: getParams() and getMethod() assume params/method exist with expected type. Missing validation can silently degrade to empty/default values and make command handling ambiguous.

4. Heap-backed DynamicJsonDocument in per-message path may increase fragmentation under frequent command traffic  
   Location: [include/jsonrpcmessage.h](esp_rgbww_firmware/include/jsonrpcmessage.h#L47), [app/jsonrpcmessage.cpp](esp_rgbww_firmware/app/jsonrpcmessage.cpp#L61)  
   Details: Embedded targets can be sensitive to repeated dynamic allocations. If message rate is high, consider static/preallocated parsing strategy.

## Low Severity

5. Public parsing/building APIs are mostly non-const, which weakens interface guarantees  
   Location: [include/jsonrpcmessage.h](esp_rgbww_firmware/include/jsonrpcmessage.h#L27), [include/jsonrpcmessage.h](esp_rgbww_firmware/include/jsonrpcmessage.h#L29), [include/jsonrpcmessage.h](esp_rgbww_firmware/include/jsonrpcmessage.h#L43), [include/jsonrpcmessage.h](esp_rgbww_firmware/include/jsonrpcmessage.h#L44)  
   Details: Methods like getRoot/getParams/getMethod could be const in several cases, improving contract clarity.

6. String literal in createNestedObject uses RAM literal instead of flash macro style used elsewhere  
   Location: [app/jsonrpcmessage.cpp](esp_rgbww_firmware/app/jsonrpcmessage.cpp#L41)  
   Details: Minor memory-style inconsistency compared to F("jsonrpc"), F("method"), F("id").

7. Header-level include appears broader than needed and increases compile coupling  
   Location: [include/jsonrpcmessage.h](esp_rgbww_firmware/include/jsonrpcmessage.h#L20)  
   Details: Including RGBWWLed/RGBWWLed.h in this header may be unnecessary for declared types and can slow builds.

8. JsonProcessor interface exposes loosely constrained request state and mixed naming consistency  
   Location: [include/jsonprocessor.h](esp_rgbww_firmware/include/jsonprocessor.h#L35), [include/jsonprocessor.h](esp_rgbww_firmware/include/jsonprocessor.h#L67), [include/jsonprocessor.h](esp_rgbww_firmware/include/jsonprocessor.h#L80)  
   Details: checkParams returns int (non-self-describing status), and parameter naming differs across overloads (root/json), which is a maintainability issue.

# LedCtrl Review Items

## High Severity

1. Potential division-by-zero in clock publish cadence
   Location: [esp_rgbww_firmware/app/ledctrl.cpp](esp_rgbww_firmware/app/ledctrl.cpp#L355), [esp_rgbww_firmware/include/ledctrl.h](esp_rgbww_firmware/include/ledctrl.h#L68)
   Details: `setClockMaster(..., interval = 0)` allows `clockMasterInterval` to be zero, but `updateLed()` evaluates `_stepCounter % clockMasterInterval`, which is undefined behavior and can crash/reset MCU.

## Medium Severity

2. Sentinel/interval type mismatch can break timing logic
   Location: [esp_rgbww_firmware/include/ledctrl.h](esp_rgbww_firmware/include/ledctrl.h#L103), [esp_rgbww_firmware/app/ledctrl.cpp](esp_rgbww_firmware/app/ledctrl.cpp#L362), [esp_rgbww_firmware/app/ledctrl.cpp](esp_rgbww_firmware/app/ledctrl.cpp#L380)
   Details: `colorMasterInterval` and `transFinInterval` are `uint32_t`, but logic uses signed-style checks (`>= 0`) and appears to expect disabled values like `-1`. With unsigned storage, `>= 0` is always true and sentinel behavior is unreliable.

3. Re-init leaks `StepSync` object
   Location: [esp_rgbww_firmware/app/ledctrl.cpp](esp_rgbww_firmware/app/ledctrl.cpp#L68)
   Details: `init()` always allocates `new StepSync()` without deleting prior `_stepSync`. If `init()` is called more than once (reinit/recovery path), heap memory leaks.

4. Pin validation can return false too early for SoC pin sets
   Location: [esp_rgbww_firmware/app/ledctrl.cpp](esp_rgbww_firmware/app/ledctrl.cpp#L212)
   Details: In `isPinValid()`, once a matching SoC entry is found, the function returns false immediately if the pin is not in that first matching entry. If config contains multiple entries for same SoC, valid pins in later entries are never checked.

5. Member state fields are not default-initialized in header
   Location: [esp_rgbww_firmware/include/ledctrl.h](esp_rgbww_firmware/include/ledctrl.h#L100)
   Details: `clockMaster`, `colorMaster`, `startupColorLast`, and several intervals have no in-class defaults. Current behavior relies on `init()->reconfigure()` ordering; using methods before full init can read indeterminate state.

## Low Severity

6. Logging call likely missing format specifier
   Location: [esp_rgbww_firmware/app/ledctrl.cpp](esp_rgbww_firmware/app/ledctrl.cpp#L478)
   Details: `debug_i("_timerInterval", _timerInterval);` passes an extra argument without `%` token, so interval value is likely not logged correctly.

7. API parameter is unused in RAW mode path
   Location: [esp_rgbww_firmware/app/ledctrl.cpp](esp_rgbww_firmware/app/ledctrl.cpp#L568), [esp_rgbww_firmware/app/ledctrl.cpp](esp_rgbww_firmware/app/ledctrl.cpp#L607)
   Details: `direction` is accepted by `setOn/setOff` but ignored for `fadeRAW(...)`, which can confuse callers expecting parity with HSV path behavior.

# mdnsHandler Review Items

## High Severity

1. Potential out-of-bounds read from non-terminated buffer in hostname sanitization path  
   Location: [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L74), [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L885), [esp_rgbww_firmware/include/mdnsHandler.h](esp_rgbww_firmware/include/mdnsHandler.h#L51)  
   Details: Both call sites copy untrusted input using `strncpy(..., sizeof(buf))` without forcing `buf[sizeof(buf)-1] = '\0'` before calling sanitizer. Sanitizer loop tests `*read_ptr != '\0'` and can read past buffer end if no terminator is present.

2. Discovery schema mismatch: swarm parser expects `type=host`, swarm advertiser never publishes `type`  
   Location: [esp_rgbww_firmware/include/mdnsHandler.h](esp_rgbww_firmware/include/mdnsHandler.h#L218), [esp_rgbww_firmware/include/mdnsHandler.h](esp_rgbww_firmware/include/mdnsHandler.h#L274), [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L251), [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L304), [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L312)  
   Details: Search targets swarm service ([esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L449)), but swarm TXT only includes id/leader/groups. Parser requires `type=host` to add controllers, so host discovery can be dropped.

3. Handler lifecycle leak risk: class registers itself in mDNS server but destructor does not unregister self  
   Location: [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L163), [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L42)  
   Details: `mDNS::server.addHandler(*this)` is called, but destructor removes only responder handlers, not `*this`. If object is destroyed/recreated, stale handler pointer risk exists.

## Medium Severity

4. Adaptive interval is computed but not applied to active timer interval  
   Location: [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L180), [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L182), [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L463)  
   Details: `_currentMdnsTimerInterval` is changed in `onMessage`, but `_mdnsSearchTimer.setIntervalMs(...)` is only done during start. Restarting timer with `startOnce()` alone may keep old interval.

5. Variable-length stack array from network-derived name length can cause portability/stack risk  
   Location: [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L233)  
   Details: `char hostname[hostname_len + 1];` is a VLA (non-standard C++ extension) and size comes from packet content; large names can pressure stack.

6. Internal hostname state is set but not used for filtering decisions  
   Location: [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L111), [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L152), [esp_rgbww_firmware/app/mdnsHandler.cpp](esp_rgbww_firmware/app/mdnsHandler.cpp#L216)  
   Details: `searchName` is assigned and logged but routing relies on suffix checks and `service` constant; this is maintainability debt and a source of divergence bugs.

## Low Severity

7. Dead/private API declarations remain without active implementation path  
   Location: [esp_rgbww_firmware/include/mdnsHandler.h](esp_rgbww_firmware/include/mdnsHandler.h#L355), [esp_rgbww_firmware/include/mdnsHandler.h](esp_rgbww_firmware/include/mdnsHandler.h#L405), [esp_rgbww_firmware/include/mdnsHandler.h](esp_rgbww_firmware/include/mdnsHandler.h#L406)  
   Details: Ping-related members are declared but corresponding code is commented out in source, increasing confusion and drift risk.

8. Comment/config mismatch in ping interval  
   Location: [esp_rgbww_firmware/include/mdnsHandler.h](esp_rgbww_firmware/include/mdnsHandler.h#L360)  
   Details: Value is `10000` (10s) while comment says “every minute”.

# MQTT Review Items

## High Severity

1. MQTT credentials are written to logs in plain text
   Location: [esp_rgbww_firmware/app/mqtt.cpp](esp_rgbww_firmware/app/mqtt.cpp#L81)
   Details: The full URL includes username and password and is logged via debug_i. This can leak broker credentials through serial logs, remote log pipelines, or crash dumps.

2. Reconnect can continue after intentional stop due to missing run-state guard
   Location: [esp_rgbww_firmware/app/mqtt.cpp](esp_rgbww_firmware/app/mqtt.cpp#L35), [esp_rgbww_firmware/app/mqtt.cpp](esp_rgbww_firmware/app/mqtt.cpp#L47), [esp_rgbww_firmware/app/mqtt.cpp](esp_rgbww_firmware/app/mqtt.cpp#L167), [esp_rgbww_firmware/include/mqtt.h](esp_rgbww_firmware/include/mqtt.h#L80)
   Details: onComplete always schedules connectDelayed, and stop only deletes mqtt. There is no active state gate or timer cancel in stop, so reconnect attempts may still be scheduled unexpectedly.

## Medium Severity

3. Transition-finished payload field has a typo likely breaking consumers
   Location: [esp_rgbww_firmware/app/mqtt.cpp](esp_rgbww_firmware/app/mqtt.cpp#L378)
   Details: Field is published as requequed instead of requeued. Subscribers expecting the correct key will miss state.

4. URL assembly does not escape credentials
   Location: [esp_rgbww_firmware/app/mqtt.cpp](esp_rgbww_firmware/app/mqtt.cpp#L79)
   Details: Username/password are concatenated directly into mqtt://user:pass@host:port. Reserved characters such as @, :, / can corrupt URL parsing and cause connection failures.

5. Declared state/methods are not implemented or used, creating lifecycle drift
   Location: [esp_rgbww_firmware/include/mqtt.h](esp_rgbww_firmware/include/mqtt.h#L69), [esp_rgbww_firmware/include/mqtt.h](esp_rgbww_firmware/include/mqtt.h#L80)
   Details: processHACommand is declared but not implemented in the shown source, and _running is declared but not used to control start/stop/connect behavior.

6. Disconnect callback ignores completion status signal
   Location: [esp_rgbww_firmware/app/mqtt.cpp](esp_rgbww_firmware/app/mqtt.cpp#L35)
   Details: success parameter is unused; callback behavior is identical for all outcomes, reducing observability and making failure-mode handling coarse.

## Low Severity

7. Logging and comments are inconsistent with behavior in a few places
   Location: [esp_rgbww_firmware/app/mqtt.cpp](esp_rgbww_firmware/app/mqtt.cpp#L55), [esp_rgbww_firmware/app/mqtt.cpp](esp_rgbww_firmware/app/mqtt.cpp#L99)
   Details: Some messages/comments suggest connection-state assumptions that are not enforced by code paths, increasing maintenance friction.

# Networking Review Items

## High Severity

1. Wi-Fi password is logged in clear text during connect  
Location: esp_rgbww_firmware/app/networking.cpp  
Details: The log line prints both SSID and password. This is a credential leak risk via serial logs, remote logs, and crash dumps.

2. Captive DNS lifecycle is incomplete and can stay active after AP stop  
Location: esp_rgbww_firmware/app/networking.cpp, esp_rgbww_firmware/app/networking.cpp  
Details: DNS is started in startAp, but there is no matching stop call in stopAp. That can leave stale DNS behavior or resource leakage when AP is cycled.

## Medium Severity

3. Retry threshold check uses equality, which can miss failure transition  
Location: esp_rgbww_firmware/app/networking.cpp  
Details: The condition checks _con_ctr == DEFAULT_CONNECTION_RETRIES instead of >=. If callback timing or multiple disconnect reasons skip the exact value, the error branch can be bypassed.

4. AP DNS startup allocates heap timer per call with fragile cleanup path  
Location: esp_rgbww_firmware/app/networking.cpp  
Details: A new Timer is allocated each time startAp runs. It is only deleted when AP IP becomes non-zero; failure paths can leak timer allocations and callbacks.

5. Scan state can get stuck true if scan start fails immediately  
Location: esp_rgbww_firmware/app/networking.cpp, esp_rgbww_firmware/app/networking.cpp, esp_rgbww_firmware/include/networking.h  
Details: _scanning is set before startScan and only cleared in scanCompleted. If startScan fails synchronously, isScanning may report true indefinitely.

## Low Severity

6. Header exposes placeholder API that always returns empty data  
Location: esp_rgbww_firmware/include/networking.h  
Details: getLedGroups currently returns an empty vector regardless of state. This can mislead callers and hide unimplemented behavior.

7. Accessors return by value and use inconsistent naming/style  
Location: esp_rgbww_firmware/include/networking.h, esp_rgbww_firmware/include/networking.h  
Details: Minor maintainability issue: copies on every call and mixed naming conventions in public API.

# OTA Update Review Items

## High Severity

1. Partition delete helper can remove the wrong partition when target is missing  
Location: esp_rgbww_firmware/app/otaupdate.cpp, esp_rgbww_firmware/app/otaupdate.cpp, esp_rgbww_firmware/app/otaupdate.cpp, esp_rgbww_firmware/app/otaupdate.cpp  
Details: getPartitionIndex returns 0 when not found, and delPartition treats index 0 as protected/invalid. This conflates not-found with first-entry and can silently block valid operations or mis-handle table edits in transitional OTA flows.

2. Overlap validation in partition table writer is incorrect and can miss real overlaps  
Location: esp_rgbww_firmware/app/otaupdate.cpp  
Details: The check uses partition.offset + partition.size < partitionEnd. Correct overlap detection should compare current start against previous end. As written, overlapping entries may pass validation and produce a broken partition table.

## Medium Severity

3. OTA status is forced to success on every boot, masking prior states  
Location: esp_rgbww_firmware/app/otaupdate.cpp  
Details: checkAtBoot always calls saveStatus(OTA_SUCCESS), even when no OTA happened this boot. This reduces diagnostic value and can hide meaningful transitional states.

4. Fixed flash size is hard-coded during partition table rewrite  
Location: esp_rgbww_firmware/app/otaupdate.cpp  
Details: deviceHeader.size is forced to 0x00400000. On devices with different flash sizes, table metadata can become inconsistent.

5. String copies into partition entry names may be non-terminated  
Location: esp_rgbww_firmware/app/otaupdate.cpp, esp_rgbww_firmware/app/otaupdate.cpp, esp_rgbww_firmware/app/otaupdate.cpp, esp_rgbww_firmware/app/otaupdate.cpp  
Details: strncpy with full buffer length does not guarantee trailing null byte. Later String conversions/logging can read garbage beyond the name field.

6. Public inline API may call function not implemented on non-ESP8266 builds  
Location: esp_rgbww_firmware/include/otaupdate.h, esp_rgbww_firmware/include/otaupdate.h, esp_rgbww_firmware/app/otaupdate.cpp  
Details: getSpiffsPartition is always available in header and calls findSpiffsPartition, but cpp implementation is inside ARCH_ESP8266 guard. This is a portability/linkage risk if referenced in other targets.

## Low Severity

7. Conditional compilation style is inconsistent and brittle  
Location: esp_rgbww_firmware/include/otaupdate.h  
Details: Uses #if ARCH_ESP8266 instead of #ifdef ARCH_ESP8266 unlike nearby guards. Depending on toolchain defines, this can trigger warnings or unexpected behavior.

8. Typo in method name reduces API clarity  
Location: esp_rgbww_firmware/include/otaupdate.h  
Details: isProccessing is misspelled, which hurts discoverability and consistency.

# StepSync Review Items

## High Severity

1. Division-by-zero risk in steering calculation when master clock does not advance  
Location: esp_rgbww_firmware/app/stepsync.cpp, esp_rgbww_firmware/app/stepsync.cpp  
Details: masterDiff can be 0 (e.g., duplicate/stale master packet), but it is used as divisor in curSteering calculation. This can produce INF/NaN and destabilize timer control.

## Medium Severity

2. Overflow-delta helper is off by one on wrap-around  
Location: esp_rgbww_firmware/include/stepsync.h  
Details: For wrapped counters, correct delta is max - prev + cur + 1. Current code omits +1, introducing systematic sync error at wrap boundaries.

3. Unsafely narrowing uint32 deltas into signed int  
Location: esp_rgbww_firmware/app/stepsync.cpp, esp_rgbww_firmware/app/stepsync.cpp, esp_rgbww_firmware/app/stepsync.cpp  
Details: calcOverflowVal on uint32 values is stored in int. Large jumps can overflow signed int and invert control behavior.

4. Catch-up accumulator can overflow over long runtime  
Location: esp_rgbww_firmware/include/stepsync.h, esp_rgbww_firmware/app/stepsync.cpp  
Details: _catchupOffset is signed int and accumulates indefinitely. Extended drift or long uptime can overflow and corrupt steering decisions.

## Low Severity

5. Mixed float/double arithmetic and implicit truncation in interval scaling  
Location: esp_rgbww_firmware/include/stepsync.h, esp_rgbww_firmware/app/stepsync.cpp, esp_rgbww_firmware/app/stepsync.cpp  
Details: _steering is double, curSteering is float, and nextInt is uint32_t. Repeated implicit conversions can add quantization noise in tight control loops.

# Telemetry Review Items

## High Severity

1. Telemetry credentials are logged in clear text  
Location: esp_rgbww_firmware/app/telemetry.cpp, esp_rgbww_firmware/app/telemetry.cpp  
Details: URL is built as mqtt://user:pass@host and printed directly. This leaks credentials to logs and any remote log sink.

2. Reconnect gate timer callback can outlive object lifecycle  
Location: esp_rgbww_firmware/app/telemetry.cpp, esp_rgbww_firmware/app/telemetry.cpp, esp_rgbww_firmware/app/telemetry.cpp  
Details: Timer callback stores and dereferences this, but destructor does not stop/disarm the timer. If destruction occurs while timer is armed, callback may dereference a stale pointer.

## Medium Severity

3. MQTT handlers are registered after connect call  
Location: esp_rgbww_firmware/app/telemetry.cpp, esp_rgbww_firmware/app/telemetry.cpp, esp_rgbww_firmware/app/telemetry.cpp, esp_rgbww_firmware/app/telemetry.cpp  
Details: Fast connect/fail events can occur before delegates are attached, causing missed state transitions and unstable reconnect behavior.

4. Blocking delay in reconnect path can stall the event loop  
Location: esp_rgbww_firmware/app/telemetry.cpp, esp_rgbww_firmware/app/telemetry.cpp  
Details: delay(1000) blocks cooperative processing and can impact websocket/network responsiveness during reconnect storms.

5. Reconnect gate timestamp type is too narrow and signed  
Location: esp_rgbww_firmware/include/telemetry.h, esp_rgbww_firmware/app/telemetry.cpp, esp_rgbww_firmware/app/telemetry.cpp  
Details: millis() is unsigned long, but _lastReconnectAttempt is int. Long uptime can overflow/sign-wrap and break 10s gate logic.

6. Reconnect gating is inconsistent between publish overloads  
Location: esp_rgbww_firmware/app/telemetry.cpp, esp_rgbww_firmware/app/telemetry.cpp  
Details: JsonDocument publish attempts gated reconnect; payload-string publish just returns false. log() uses payload-string path, so telemetry logging may never self-recover while stats do.

## Low Severity

7. Header API naming mismatches implementation intent  
Location: esp_rgbww_firmware/include/telemetry.h  
Details: connect parameters are named debugServer/debugUser/debugPass although class is telemetry-focused, which is confusing and increases maintenance drift.

# Webserver Review Items

## High Severity

1. Basic auth check is too weak and can accept malformed credentials with password suffix match  
Location: esp_rgbww_firmware/app/webserver.cpp, esp_rgbww_firmware/app/webserver.cpp  
Details: After base64 decode, authentication uses endsWith(configuredPassword) instead of validating full user:password credentials. Inputs ending with the password can pass unexpectedly.

2. Potential invalid delete due to uninitialized websocket resource pointer  
Location: esp_rgbww_firmware/include/webserver.h, esp_rgbww_firmware/include/webserver.h, esp_rgbww_firmware/app/webserver.cpp  
Details: wsResource has no in-class initialization and is deleted in destructor. If init() was never called, pointer value may be indeterminate.

3. Wi-Fi credentials are logged in plain text via HTTP connect endpoint  
Location: esp_rgbww_firmware/app/webserver.cpp  
Details: Password is printed in debug logs (ssid/pass), leaking secrets into local and remote log sinks.

## Medium Severity

4. Possible out-of-bounds access on empty URI path in static file handler  
Location: esp_rgbww_firmware/app/webserver.cpp, esp_rgbww_firmware/app/webserver.cpp, esp_rgbww_firmware/app/webserver.cpp  
Details: fileName[0] is read without checking fileName length.

5. Response status handling collapses all non-OK codes to 400  
Location: esp_rgbww_firmware/app/webserver.cpp, esp_rgbww_firmware/app/webserver.cpp  
Details: sendApiResponse ignores passed status and forces HTTP_STATUS_BAD_REQUEST for any non-OK case, reducing protocol correctness.

6. Invalid/odd HTTP header used for auth failure  
Location: esp_rgbww_firmware/app/webserver.cpp  
Details: Header name "401 wrong credentials" is non-standard and includes spaces; behavior across clients/proxies is unpredictable.

7. CORS allow-headers omits Authorization, breaking secured browser clients  
Location: esp_rgbww_firmware/app/webserver.cpp  
Details: With API security enabled, browser preflight for Authorization may fail because header is not allowed.

8. JSON deserialization errors are ignored in multiple command handlers  
Location: esp_rgbww_firmware/app/webserver.cpp, esp_rgbww_firmware/app/webserver.cpp, esp_rgbww_firmware/app/webserver.cpp  
Details: Requests can proceed with partially parsed/default data, increasing risk of unintended commands.

## Low Severity

9. Dead declaration inside OTA branch in index handler indicates merge artifact  
Location: esp_rgbww_firmware/app/webserver.cpp  
Details: Stray declaration inside onIndex block suggests accidental paste and should be removed for clarity.

# WebSocket Keepalive Review Items

## High Severity

1. WebSocket broadcast currently targets only the first connected client
Location: esp_rgbww_firmware/app/webserver.cpp
Details: wsSendBroadcast uses webSockets[0] and does not iterate all clients, so only one client reliably receives updates.

2. EventServer keepalive timer is not stopped during shutdown
Location: esp_rgbww_firmware/app/eventserver.cpp, esp_rgbww_firmware/app/eventserver.cpp
Details: keepalive timer is started in start but stop does not stop it before shutdown, risking callbacks after service teardown.

3. Authentication check is too permissive for Basic auth payload validation
Location: esp_rgbww_firmware/app/webserver.cpp, esp_rgbww_firmware/app/webserver.cpp
Details: decoded authorization is validated with endsWith(password), which is weaker than full user:password match.

## Medium Severity

4. Service lifecycle teardown is incomplete across AP/STA/OTA transitions
Location: esp_rgbww_firmware/include/application.h, esp_rgbww_firmware/app/networking.cpp, esp_rgbww_firmware/app/otaupdate.cpp
Details: start paths are present, but missing centralized stopServices implementation/use can leave sockets and timers active during mode transitions and reboot paths.

5. CORS allow-headers omit Authorization
Location: esp_rgbww_firmware/app/webserver.cpp
Details: browser preflight may fail for secured requests when Authorization header is required.

6. Credentials are logged in plaintext in websocket-adjacent paths
Location: esp_rgbww_firmware/app/webserver.cpp, esp_rgbww_firmware/app/telemetry.cpp
Details: Wi-Fi and telemetry credentials can leak into logs.

## Low Severity

7. Keepalive cadence is inconsistent between HTTP server and EventServer
Location: esp_rgbww_firmware/app/webserver.cpp, esp_rgbww_firmware/include/eventserver.h
Details: different intervals make stale-client handling behavior inconsistent across transports.



## High Severity

1. WebSocket broadcast currently targets only the first connected client  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L119)  
   **Details:** `wsSendBroadcast` uses `webSockets[0]` and does not iterate all clients, so only one client reliably receives updates.

2. EventServer keepalive timer is not stopped during shutdown  
   **Location:** [app/eventserver.cpp](app/eventserver.cpp#L52), [app/eventserver.cpp](app/eventserver.cpp#L60)  
   **Details:** Keepalive timer is started in `start()` but `stop()` does not stop it before `shutdown()`, risking callbacks after service teardown.

3. Authentication check is too permissive for Basic auth payload validation  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L173)  
   **Details:** Decoded authorization is validated with `endsWith(password)`, which is weaker than a full `user:password` match. (See also Webserver H1.)

## Medium Severity

4. Service lifecycle teardown is incomplete across AP/STA/OTA transitions  
   **Location:** [include/application.h](include/application.h#L49), [app/networking.cpp](app/networking.cpp#L240), [app/otaupdate.cpp](app/otaupdate.cpp)  
   **Details:** Start paths are present, but missing centralized `stopServices()` implementation can leave sockets and timers active during mode transitions and reboot paths.

5. CORS allow-headers omit `Authorization`  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L1594)  
   **Details:** Browser preflight may fail for secured requests when `Authorization` header is required. (See also Webserver M7.)

6. Credentials are logged in plaintext in websocket-adjacent paths  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L1190), [app/telemetry.cpp](app/telemetry.cpp#L85)  
   **Details:** Wi-Fi and telemetry credentials can leak into logs. (See Webserver H3 and Telemetry H1.)

## Low Severity

7. Keepalive cadence is inconsistent between HTTP server and EventServer  
   **Location:** [app/webserver.cpp](app/webserver.cpp#L48), [app/eventserver.cpp](app/eventserver.cpp#L52)  
   **Details:** HTTP server keepalive is 10 s; EventServer keepalive is 60 s. Different intervals make stale-client handling behavior inconsistent across transports.