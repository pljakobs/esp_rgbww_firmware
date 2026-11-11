# Experimental Branch Remediation Plan

## 0. Prime directive

**This system must be rock solid.** Every decision in this plan is subordinate to
these non-negotiable goals:

1. **No uncommanded reboots.** The controller must stay up; unexpected resets are
   the top-priority defect.
2. **The webapp must load reliably and fast.** Serving the UI must not be starved
   by heap pressure or connection limits.
3. **OTA must be 100% trustworthy.** Firmware and webapp updates must never brick
   a device or leave it in an inconsistent state.

Anything that trades against stability, webapp responsiveness, or OTA integrity
is not worth shipping, regardless of feature value.

## 1. Background & goal

The `experimental` branch was created to move the webapp out of the firmware
binary (FSTR/`fileMap`) and into the LittleFS file system, served from flash and
updated independently of the firmware via an OTA process. **That core goal
works.**

Along the way, a large amount of other work accumulated on the branch (API
refactor, webserver rework, heap hardening, CI/test infrastructure, crash
reporting). The branch has diverged far enough from `develop` that it can no
longer be merged as-is, and it has become unstable.

The standout architectural outcome of this stack is the **API handler
abstraction**: it decouples the API business logic from the transport layer
(HTTP / WebSocket / MQTT), so all three protocols share one dispatch path and
clients can reach every verb over any avenue. This is the single most valuable
result of the whole effort and is the change we most want to preserve — see §4.B.

## 2. Current status

### Works
- **API handler abstraction** — protocol-agnostic dispatch shared by HTTP,
  WebSocket and MQTT (the major architectural win of this stack).
- General webapp update process (LittleFS staging + activation).
- `mdnsHandler` rework: dropped group announcement.
- `mdnsHandler` rework: dropped dead ping code.
- Webapp-version mDNS announcement + compatible-controller discovery.

### Broken / regressed
- **System is unstable**: crashes within seconds of boot, often with no reason
  printed on the console.
- **Heap usage is spiky** and differs significantly between controllers.
- **Loading the webapp frequently fails** — not enough free HTTP connections /
  too little free heap.

## 3. Divergence overview

| Metric | Value |
| --- | --- |
| Merge base | `e064a6d` |
| Commits on `experimental` not on `develop` | **192** |
| Files changed | 58 |
| Line delta | **+9,904 / −1,925** |

The sheer size of the divergence is itself a problem: too many entangled changes
land at once, so a plain `git bisect` across the whole range is impractical and
the true root cause of the instability has never been isolated — only its
symptoms have been repeatedly patched.

## 4. Changes between `develop` and `experimental`

Grouped by theme. **Status** reflects runtime health; **Port** is the
recommendation for bringing the change back to `develop`.

### A. Webapp → LittleFS + OTA  *(the secondary goal)*
- New `app/webappota.cpp` (+908) and `include/webappota.h`; LittleFS staging,
  free-space verification, stale-file purge, retry-from-clean-staging.
- `webapp/captive.html`, `webapp/updating.html`, minimal embedded fallback set
  (`include/fileList.h`).
- Deploy restructuring (`manage_version.py`, `deploy`, pages workflow) with
  webapp version tracking and `download/firmware/` layout.
- Key commits: `701c44e`, `9823e2d`, `9a8edc0`, `3f56fd5`, `2d79fee`, `206b6c3`.
- **Status:** works. **Port:** yes — this is the feature we want to keep.

### B. API handler abstraction *(the primary goal — major win)*
- New `app/apihandler.cpp` (+632) / `include/apihandler.h`: a protocol-agnostic
  marshaller. HTTP, WebSocket and MQTT requests are routed through one dispatch;
  action verbs go to `JsonProcessor`, `info`/`hosts` handled directly.
- Decouples API business logic from transport: a single source of truth for the
  API surface, every verb reachable over HTTP/WS/MQTT, and a natural seam for
  host-emulator testing of the API without a network stack.
- Touches `jsonprocessor.cpp`, `mqtt.cpp`, `webserver.cpp`.
- Key commits: `033b5d1`, `bbd77f3`, `c575fab`, `fd36e5a`, `e5e7e2c`.
- **Status:** works and is the change with the highest long-term value.
  Caveat: it currently sits on the hot path of the suspect webserver rework
  (group C), so it must be ported *cleanly separated* from that churn.
- **Port:** yes — treat as the **keystone** of the port. Landing it early on
  `develop` also decouples the API from the transport code, which makes the
  group-C rework far easier to reason about and heap-test.

### C. Webserver rework & OOM hardening  *(highest churn: +856/−492)*
- Repeated attempts to survive low-heap conditions: load shedding during webapp
  fetch, inbound-connection clamping, HTTP 307 redirect of clients to a peer
  controller when heap is low, CORS/preflight cleanup, JSON body parser rewrites.
- Connection limits were raised and lowered several times (8 → 4 → 3, TX buffer
  512, serialize HTTP connections on ESP8266).
- Key commits: `71c4bbb`, `45af51b`, `139f100`, `e9eda58`, `d901d81`, `952475e`,
  `095c557`, `61bc17c`, `e9256fe`, `43af575`.
- **Status:** **suspect.** This is where most instability firefighting happened;
  the churn pattern suggests symptoms were treated, not the cause.
  **Port:** do **not** bulk-port. Extract only individually justified fixes.

### D. mDNS handler rework
- Drop group announcement (`1eab479`), drop dead ping code (`b55fe98`,
  `45930e0`), webapp-version announcement (`5461773`, `6184a2a`), host-type
  propagation (`cb1bee2`).
- **Status:** works. **Port:** yes — clean, self-contained, high value.

### E. Crash reporting & diagnostics
- Crash reporter rewrite, `reportCrashDump()` at bringup, larger coredump
  partition, RTC crash-dump capture.
- Crash-loop rollback to the other ROM (PR #144, `3b61e20`) — separate feature.
- Key commits: `723d505`, `a151c20`, `b407e7d`, `8a12c3a`, `4d08f29`.
- **Status:** works / diagnostic aid. **Port:** yes — these will also *help
  diagnose* the instability (that's the immediate value).

### F. Heap / memory optimizations
- EventServer per-event heap churn removed (`be0cb96`), telemetry credential
  buffers dropped (`8af2beb`), string literals moved to flash (`dda6f06`),
  RGBWWLed submodule shrink (~3.1 KB/channel, `a140f5a`), `minHeap` switched to
  `uint` and floored at `0x80000`.
- **Status:** good, low risk. **Port:** yes.

### G. Partition / hardware config
- ESP32 single larger LittleFS partition (needed because webapp staging must fit
  the bigger 1.4 MB ROMs), removed `factory` partition, `buildmap` target,
  coredump partition growth.
- Key commits: `99a06b4`, `1ddbdc7`, `dacb148`, `d7a9d5b`.
- **Status:** required by the webapp-in-LFS feature. **Port:** yes, together with
  group A. **Risk:** partition-map changes interact with OTA — handle carefully.

### H. MQTT fixes
- DNS-callback crash on reconnect (`29eeba6`), deferred reconnect via timer to
  avoid use-after-free (`ef4805a`).
- **Status:** genuine crash fixes. **Port:** yes.

### I. Auth (good, works)
- Shared-password API authentication with cached security/password to avoid
  ConfigDB reads on the request hot path (`9e1368e`, `7148f4e`).
- **Status:** works. **Port:** yes.

### J. CI / host-emulator test infrastructure  (not yet fully working as file system root access fails) *(large churn, no device runtime impact)*
- Host smoke tests migrated to pytest, valgrind snapshots, lwIP PCB/MSL host
  overrides, container/dependency fixes, backup copies of old test scripts.
- **Status:** neutral for device stability. **Port:** selectively / can wait.

## 5. Instability hypotheses (ranked)

1. **Heap pressure from serving the webapp out of LittleFS.** Serving files +
   staging area + more concurrent HTTP connections raises peak heap use; on a
   fragmented heap this tips into OOM and hard faults. *Fits "spiky heap",
   "webapp load fails", "differs between controllers" (different flash/heap
   states).*
2. **lwIP / TCP PCB exhaustion on ESP8266.** Multiple commits fight
   `ip_input` crashes and serialize connections; suggests the network stack is
   being pushed past its PCB pool under webapp load. *Fits "crash seconds after
   boot", "not enough HTTP connections".*
3. **Boot-time work racing the watchdog.** LittleFS mount + webapp version check
   + OTA gating happen early; a slow/large FS op could trip the WDT. *Fits
   "crashes seconds after boot with no reason".*
4. **Use-after-free / lifetime bugs** introduced by the API/webserver refactor
   (several already-fixed String `c_str()` lifetime and MQTT reconnect bugs hint
   more may remain).

## 6. Remediation strategy

**Do not try to stabilize `experimental` in place.** Instead, rebuild the
feature on top of a known-good `develop` in reviewable increments:

1. **Port the safe, self-contained wins first** (groups D, F, H, I, E) so
   `develop` gains value immediately and gains the crash-diagnostic tooling.
2. **Port the API handler abstraction (group B) as the keystone.** Lift it out
   of the group-C webserver churn and land it on `develop` as its own reviewed
   change. This is the primary win to preserve, and decoupling the API from the
   transport makes every later step (webapp feature, webserver rework) cleaner.
3. **Land the webapp-in-LFS feature as a focused stack** (groups A + G), each PR
   small enough to review and heap-test on device.
4. **Re-derive the webserver changes (group C) from scratch** against the
   crash-diagnostics and the now-decoupled API, keeping only fixes with a proven
   root cause. Treat the existing group-C churn as a list of *symptoms to
   explain*, not patches to copy.
4. **Instrument before optimizing:** with `reportCrashDump()` + crash-loop
   rollback in place, capture real crash reasons and per-controller heap traces
   to confirm/refute the hypotheses in §5 before writing more fixes.

## 7. Tasks

- [ ] Review all 192 commits `origin/develop..origin/experimental` and tag each
      as *port* / *rework* / *drop* (use the grouping in §4 as the starting map).
- [ ] Port the two `mdnsHandler` reworks (group D) to `develop`.
- [ ] Port the heap optimizations (group F) and MQTT crash fixes (group H).
- [ ] Port crash reporting + crash-loop rollback (group E) to get diagnostics on
      `develop`.
- [ ] **Port the API handler abstraction (group B) as its own PR**, separated
      from the group-C webserver churn — the keystone of the port.
- [ ] Reproduce the instability with diagnostics enabled; record actual reset
      reasons and heap-at-crash per controller.
- [ ] Confirm the leading root cause (heap vs lwIP PCB vs WDT) from §5.
- [ ] Rebuild the webapp-in-LFS feature (groups A + G) as a small, heap-tested
      PR stack onto `develop`, on top of the ported API handler.
- [ ] Decide fate of the CI/test churn (group J) — port selectively.

## 8. Full commit inventory (grouped)

Every non-merge commit in `origin/develop..origin/experimental` (192 total,
newest first within each group). Descriptions are condensed from the commit
subjects. A handful that don't map cleanly are listed under *Misc*.

### A. Webapp → LittleFS + OTA
- `e9256fe` — delay webapp OTA 10 s after `staGotIP`, gate on ≥17000 B free heap
- `ee3d4ba` — remove directory listing at boot time
- `9324712` — robust staging cleanup + free-space verification
- `2d79fee` — recover interrupted webapp OTA by retrying from clean staging
- `86f44f2` — add filesystem stats to `/info` API endpoint
- `9a8edc0` — hardcoded free-space check in `webappOta`
- `9823e2d` — add filesystem stats to `WebappOta`
- `9e28108` — exclude directory listing when running on host
- `17ed1cb` — list full directory layout before `webappOta` (residual-file check)
- `96321e0` — avoid leading slash in `moveTree` paths when `dstDir` is root
- `f58ef55` — add LittleFS free/used/total to v2 `/info` response
- `3f56fd5` — purge stale webapp files before activating new version
- `7102e07` — add `ignoreEnabled` param to `checkForUpdate()`
- `6132bfe` — add `POST /webapp_check` endpoint and `webapp_check` API verb
- `55e5ab0` — track `include/fileList.h` in git (minimal embed set)
- `42354b6` — add `webappota.h` / `webappota.cpp` source files
- `b4bfe7c` — commit `captive.html` / `updating.html` to firmware repo
- `c1bf3fa` — fall back to `raw.githubusercontent.com` for captive/updating html
- `0d9badd` — fix unzip paths for experimental minimal extract
- `887351c` — experimental branch uses minimal embedded file set
- `67eaca7` — add `type`/`min_firmware` to webapp entries, drop `base_url`
- `701c44e` — implement background webapp OTA with LittleFS staging
- `f373337` — deploy: run port knock before SSH host-key scan
- `0e0fbc9` — deploy: bootstrap `download/firmware` paths, align latest symlink
- `206b6c3` — deploy: add webapp version tracking, restructure to `download/firmware/`

### B. API handler abstraction *(keystone)*
- `53841f6` — fix WebSocket JSON-RPC method error responses
- `5bd5f4d` — centralize webserver JSON parsing and require API dispatch
- `137e050` — websocket: return JSON-RPC result payloads for `getColor`/command methods
- `5795f18` — websocket: restore JSON-RPC request handling; relative redirect `Location`
- `c76ccc5` — strict JSON parse validation + fix WebSocket broadcast
- `e5e7e2c` — refactor API command dispatch + host info comparison tools
- `07535c9` — fix `Json::deserialize` return handling in `JsonRpcMessageIn`
- `d79d5c5` — handle malformed JSON explicitly for WebSocket and API
- `bbd77f3` — centralize firmware API dispatch and websocket handlers
- `033b5d1` — `apihandler` marshaller (HTTP/WS/MQTT routed through one dispatch)
- `c575fab` — initial `apihandler` to separate API business logic from webserver
- `fd36e5a` — initial plan for protocol-abstracted API handler class

### C. Webserver rework & OOM hardening *(suspect)*
- `43af575` — delay webserver startup a few seconds
- `61bc17c` — HTTP 307 redirect to peer controller when heap low
- `b0b2516` — remove tcp state counters
- `2652454` — set esp32/esp32c3 connection limits higher than esp8266
- `d901d81` — lower `WEBSERVER_MAX_CONN_DEFAULT` 4→3
- `45af51b` — clamp inbound connections during webapp OTA
- `6f4a3c3` — copy parse-error string into JSON response
- `71c4bbb` — shed load during webapp fetch to prevent OOM
- `3919ab3` — heap-allocated `DynamicJsonDocument` for request parsing
- `e5e8512` — eliminate infinite recursion in `parseJsonBody`
- `fcff0f7` — stream-first JSON parsing, `/info` cache, reuse color doc
- `3e191d7` — restore body parser for `application/json` only
- `2ba46f0` — avoid `JsonObjectStream` in `GET /color`
- `e9eda58` — serialize HTTP connections to reduce lwIP `ip_input` crashes
- `cb01d71` — parse body via char buffer+length (avoid ArduinoJson String-reader crash)
- `dbc3680` — simplify OPTIONS/method path under low heap
- `139f100` — remove global `bodyToStringParser` workaround
- `2f517e4` — remove duplicate `setCorsHeaders` calls
- `5164c29` — defer header commit until preflight validation passes
- `3b77e7a` — guard allocation-sensitive response paths
- `23d0bd4` — guard `JsonObjectStream` allocation in error responses
- `659c4c1` — use `HttpHeaders` accessor for content-length
- `47f6f13` — avoid blocking JSON stream fallback on malformed bodies
- `3129171` — restore structured JSON-RPC errors using flash literals
- `e674eb5` — remove orphaned `crashTestTimer` stop, fix `MAX_HTTP_CONNECTIONS`, webserver cleanup
- `952475e` — raise HTTP max connections to 8
- `095c557` — reduce Serial TX buffer to 512 B, replace `printf` with `debug_i`
- `8df2e05` — fix temporary String `c_str` lifetime
- `c688486` — include tcp per-state diagnostics in `/info`
- `6c27918` — increase tcp connection limits

### D. mDNS handler rework
- `45930e0` — remove ping protocol remnants from `Controllers`
- `b55fe98` — remove dead ping code from `mdnsHandler`
- `1eab479` — stop advertising group membership over mDNS
- `6184a2a` — add webapp version to controller mDNS announcement + compatible discovery
- `5461773` — add webapp version announcement to mDNS API service
- `cb1bee2` — host-type propagation across mDNS and hosts API

### E. Crash reporting & diagnostics
- `3b61e20` — crash-loop rollback to other ROM
- `723d505` — rewrite the crash reporter
- `8a12c3a` — increase coredump partition
- `a151c20` — add crash reporting entry points
- `b407e7d` — call `reportCrashDump()` in app bringup
- `486c680` — call `reportCrashDump()` in app bringup (duplicate)
- `4d08f29` — debug output around gathering reset info
- `6738c39` — re-enable pre-network Huffman buffer so boot crash dumps reach syslog
- `1aed791` — rename `UDP_DEBUG`→`RSYSLOG` (lwIP clash), enable RSYSLOG in debug
- `d92ea08` — remove stale commented-out ROM-switch logic in `switchRom()`
- `688038b` — move `debugStream` definition before `Application`
- `41377ad` — add forward declarations for `debugStream` (linker scope)
- `587605b` — revert GPIO0 crash-test trigger
- `c49c987` — experiment: disable pre-net Huffman buffer for heap validation

### F. Heap / memory optimizations
- `be0cb96` — eliminate per-event heap churn in EventServer
- `76e0ba2` — aggressive heap saving across API and network layers
- `f04d3b9` — refine API/websocket memory usage and telemetry reporting
- `d71c2ae` — fix HSV lifetime and remove EventServer interval gating
- `32eb75a` — gate `malloc_count`, zero-heap `/info` fragments
- `b336aad` — remove `malloc_count` instrumentation
- `8d93f77` — apply default heap floor when `minHeap ≤ 0`
- `8af2beb` — drop long-lived telemetry credential buffers
- `dda6f06` — move string literals to flash
- `7d2a524` — switch `minHeap` to `uint` to avoid sign errors
- `936dff6` — floor `minimumHeap` at `0x80000` (esp32 correctness)
- `a229af5` — shorten `minfreeHeap` JSON field name
- `9cc26bd` — shorten `minfreeHeap` JSON field name (duplicate)
- `da572d2` — fix (minHeap/heap-accounting context)
- `a140f5a` — bump RGBWWLed (shrink per-channel heap ~3.1 KB)
- `418d859` — bump RGBWWLed (fix mismatched `delete[]` in `AnimationQ` destructor)
- `1c2cae2` — bump RGBWWLed (fix destructor channel cleanup)
- `b1063ea` — bump RGBWWLed for host raw-output state consistency
- `f648dc7` — bump RGBWWLed (fix Host build macro)
- `de51282` — update RGBWWLed to 0.10.0
- `63db223` — bump RGBWWLed submodule
- `0b4742d` — update RGBWWLed submodule
- `fc7f563` — update RGBWWLed submodule

### G. Partition / hardware config
- `99a06b4` — ESP32 single larger LFS partition (fits webapp staging)
- `dacb148` — delete unused `factory` partition (Esp32)
- `1ddbdc7` — remove default `factory` partition (Esp32)
- `d7a9d5b` — switch to `buildmap` target for partition table
- `d6f66d9` — annotate known partition-table validation bugs with TODOs
- `f2f0890` — add esp32c6 platform

### H. MQTT fixes
- `ef4805a` — defer MQTT reconnect via timer (prevent use-after-free)
- `29eeba6` — prevent DNS callback crash on reconnect lifecycle

### I. Auth
- `9e1368e` — shared-password API authentication
- `7148f4e` — cache API security/password to avoid cfgdb reads on request hot path

### J. CI / host-emulator test infrastructure
- `14f4a6f` — avoid malformed-json body stalls, harden retry flow
- `1598990` — fallback when valgrind crashes, ignore core artifacts
- `6ba1189` — fix valgrind log path, harden pytest bootstrap
- `e5f942d` — run emulator from Host debug dir
- `de3ab41` — remove `repository_dispatch` trigger
- `f78156d` — fix `workflow_dispatch` branch detection
- `3f4aac2` — remove esp32c6 from SOC matrix
- `ed49f8e` — don't `rm webapp/` before experimental build
- `7c3b5b0` — add `experimental-externalUI` trigger + webapp branch map
- `48a1313` — Node24 compatibility for GitHub Actions
- `a141c82` — remove unused host `lwipopts`
- `6703277` — re-add advanced test cases
- `510ac8e` — fix Esp8266 lwIP flags and host smoke coverage
- `89daad0` — match malformed JSON error text expected by smoke test
- `843f53e` — fix Host build by aligning `parseJsonBody` message type
- `1924979` — restore `HOST_CI_BUILD_ONLY` flow
- `d72cf96` — reset host CI/tests to develop baseline with backups
- `13be985` — normalize rgbww allowed-warning matching
- `2bd7eee` — require ping+color readiness before each test
- `48b1d92` — make RGBWW host configurable for host emulator
- `71911f2` — restart host emulator for each pytest case
- `b51dc59` — host-only lwip PCB and MSL overrides
- `14cf35d` — override TCP PCB pool and MSL via app-level `lwipopts.h` (Host)
- `5e78073` — patch lwIP `lwipopts.h` before Host build
- `c295b01` — add apt-utils, fix `ensure_pytest` check
- `e1eeedf` — pre-install pytest/requests via apt
- `178d3d9` — add `lib32stdc++6`/`lib32z1` for 32-bit host firmware
- `c25d79b` — skip valgrind/ip/tun when `HOST_CI_BUILD_ONLY=1`
- `a9c581e` — fix artifact download path
- `67096a0` — fix host-test slim container and artifact paths
- `256f516` — decouple host build and test into separate jobs
- `e8144f3` — restore test timing calibration and allowlist
- `96b7cee` — repair host state refactor compile errors
- `20eb353` — clean up host state and ci smoke path
- `7016061` — remove `requests` dependency from rgbww smoke test
- `bfa64dd` — make rgbww smoke test py3 compatiblesecondary
- `ef3f7c5` — classify known host-emulator regressions as warnings
- `357b795` — scope format-error flags to app component
- `46f29cd` — capture pytest exit codes before classification
- `c8786e7` — allow flaky websocket RGBWW warnings
- `92d1114` — allow known host RGBWW flakes
- `6af82f1` — dual-host `/info` CSV capture utility
- `24cafcc` — surface warnings, allow known fade flakiness
- `d303e15` — CI tags for local version traceability
- `21b46f4` — fix valgrind suppression pattern (AppWIFI WVector)
- `6f267b6` — add valgrind suppression for AppWIFI/WiringList interior-pointer
- `8f48a63` — runtime valgrind startup snapshots and deltas
- `09b0400` — preinstall valgrind debuginfo
- `6d6b6bf` — host valgrind logs, harden restart teardown
- `4e64592` — relax rgbww tolerance to ±5 for DITL
- `b6433d4` — port test reporting improvements from develop
- `6dc9653` — iteration trigger for test collection
- `26ea3cd` — add HTTP redirect following to `http_request` helper
- `fb411fd` — fix smoke pytest annotation compatibility
- `769c569` — migrate experimental host smoke to pytest
- `7a10f0b` — stabilize Host smoke `/color` setter assertions
- `75c8cd9` — accept WS method-not-found error variants
- `54f2203` — improve host-smoke CI step logging
- `681c418` — harden host-smoke artifact collection
- `c771470` — host emulator tests and CI improvements
- `a2e10f6` — refine firmware branch-based version numbering
- `3cc5bd4` — sync firmware builds with websocket docs and branch artifacts

-> for proper host valgrind testing, we need to fix the filesystem situation

### Misc / uncategorized
- `1b5ef9f` — update `RGBWWCtrl.h`
- `a9489b7` — add dark-mode schema itemsecondarysecondary

## 9. Host-emulator debugging toolkit (stashed ideas)

Notes captured while enabling faithful Host-emulator testing. These are
investigation aids for the §5 instability hunt, not yet implemented.

### 9.1 Real LittleFS on the Host emulator *(done)*
- `Application::init()` / `mountfs()` now mount the real LittleFS on the Sming
  Host emulated flash (backing file `out/Host/debug/firmware/flash.bin`) instead
  of the host-OS passthrough (`IFS::Host`). This exercises the same FS + OTA code
  path as the device and is a prerequisite for meaningful valgrind runs.
- Flash file lifecycle: `make SMING_ARCH=Host flashinit` (blank), `flash` (writes
  partition table + FS image). LittleFS auto-formats a blank partition on first
  mount. `lfs0` `filename` is empty in `Lightinator_Host.hw`, so a fresh flash
  yields an empty FS (ConfigDB files are recreated by the app); to preload the
  webapp, point `lfs0.filename` at a files dir and run `make flashfs`.
- The Host build derives its OTA branch from `git describe`, which carries the
  local working branch (e.g. `rollback/979`) that has no published artifacts;
  `WebappOta::extractBranch()` now forces `experimental` under `ARCH_HOST`.

### 9.2 Can the Host emulator mirror the ESP8266 memory layout?
- **Not faithfully** — the Host target is a native x86-64 build, not an ESP8266
  CPU/memory simulator.
- **Heap**: `Arch/Host/Components/heap/heap.cpp` reports a *synthetic* figure —
  `system_get_free_heap_size() = 128 KB (notional) − MallocCount::getCurrent()`.
  It is glibc `malloc` wrapped by `malloc_count`, **not** umm_malloc. We *can*
  lower the notional ceiling or call `MallocCount::setAllocLimit(N)` to force an
  ESP8266-like OOM budget, but umm_malloc **fragmentation** (the "largest free
  block" failure mode) cannot be reproduced — that only shows on-device.
- **Stack**: the app runs on the host thread with a multi-MB stack, so a 4 KB
  `CONT_STACKSIZE` overflow does not fault. Forcing it means running the Sming
  loop on a `pthread` with `pthread_attr_setstacksize(4096)` (invasive hostlib
  patch). Accurate portable substitute: compile-time `-Wframe-larger-than=256` /
  `-fstack-usage`.
- **Section layout** (IRAM/DRAM/rodata, `0x3FFE…`): not mirrorable, irrelevant
  natively.

### 9.3 Live runtime heap/stack info
- **Valgrind caveat**: every tool profiles the *host* glibc heap and host thread
  stack, never umm_malloc / the 4 KB cont stack — so it cannot show *device*
  fragmentation. `testnet.sh` currently uses `memcheck` (leaks/invalid access
  only, no live stats).
- Better valgrind tool for this: `--tool=massif --stacks=yes` → time-series of
  heap + stack with per-callsite allocation trees (`ms_print`). Absolute limits
  are host-sized, but *allocation sizes and stack depth per call site map back to
  the device*. `--vgdb=yes` + `vgdb` allows on-demand `monitor` snapshots.
- **Best fit, already linked in**: `malloc_count` (`Components/malloc_count`)
  exposes `getCurrent/getPeak/getTotal/getAllocCount`, `resetPeak`,
  `setAllocLimit(bytes)`, `setCallback(cb)` (fires on every alloc change),
  `enableLogging`. A small `ARCH_HOST`-only telemetry hook can stream live
  peak/alloc-count with no valgrind overhead and cap the budget for OOM repro.
- **True umm fragmentation** must be measured on-device (`system_get_free_heap_size`
  + a largest-free-block query) surfaced over existing telemetry.

### 9.4 Recommended toolkit (in priority order)
1. `malloc_count` live heap telemetry hook (Host-only) + optional
   `setAllocLimit` to an ESP8266-like budget — cheap, already available.
2. Compile-time `-Wframe-larger-than` / `-fstack-usage` for stack pressure.
3. A `massif --stacks=yes` companion run to `testnet.sh` for allocation
   hot-spots and stack high-water.
4. On-device largest-free-block telemetry for real fragmentation.