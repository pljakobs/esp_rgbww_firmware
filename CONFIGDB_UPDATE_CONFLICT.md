# ConfigDB copy-on-write import failure

## Status

Investigation only; further ConfigDB fixes are deferred.

A ConfigDB-only regression reproduces `UpdateConflict`. Its connection to the
reported virtual-light exerciser failure remains unconfirmed.

Affected revision tested: `pljakobs/ConfigDB`
`889690fc1aa89acb80854bc3be34294ecbda031d`.

## Observed firmware symptoms

Running `tests/virtual_light_exerciser.py` against an ESP32-C3 discovered four
virtual lights. One invocation ended with an empty `error:` message. Another
successfully subscribed to the WebSocket `lights` channel, then:

- The first HTTP colour command was accepted but no matching state event arrived
  and `GET /color` still returned the old value.
- Subsequent HTTP colour commands returned HTTP 400:

  ```json
  {"error":"Invalid JSON: UpdateConflict"}
  ```

In this run, colour commands used HTTP. WebSocket was used for subscription and
receiving notifications, not for sending colour commands.

The empty error and the initially accepted command not taking effect are not
explained by the isolated ConfigDB regression.

## Independently reproduced dependency defect

ConfigDB stores use a weak self-reference to obtain a strong reference through
`Store::lock()` and `Store::lockForUpdate()`.

In the tested revision, `Database::loadStore()` initialises that reference, but
the copies created by `Database::lockStore()` and `Database::openStore()` do not.
Consequently, a copied store can have a valid updater while constructing an
`ObjectUpdateRef` from one of its objects fails. The JSON write-stream constructor
then reports `Error::UpdateConflict`.

Relevant implementation:

- `ConfigDB/src/Database.cpp`: store creation and both copy paths.
- `ConfigDB/src/include/ConfigDB/Store.h`: `lock()` and `lockForUpdate()`.
- `ConfigDB/src/ObjectRef.cpp`: construction of `ObjectUpdateRef` from an object.
- `ConfigDB/src/include/ConfigDB/Json/WriteStream.h`: failed reference becomes
  `UpdateConflict`.

These paths are relative to the ConfigDB repository, locally located at
`/opt/sming/Sming/Libraries/ConfigDB`.

### Minimal reproduction

Using the ConfigDB host test database:

```cpp
TestConfig::Root root(database);
ConfigDB::ObjectRef snapshot(root);
REQUIRE(snapshot);

auto updater = root.update();
REQUIRE(updater);

REQUIRE(importObject(updater, F("{\"simple-bool\":true}")));
```

Retaining `snapshot` forces copy-on-write when opening the updater. On the tested
unmodified revision, the import fails with:

```text
JSON load '': UpdateConflict
FAIL in `void StreamingTest::copyOnWriteImport()`
```

This reproduction does not involve PWM, firmware task scheduling, HTTP, or
WebSocket.

As a diagnostic experiment, initialising each copied store's weak self-reference
made the regression pass. The test also checked preservation of the old snapshot
and validity of a reader copied while an updater remained alive. All eight
ConfigDB host test groups then passed with zero failures.

Host validation command:

```sh
timeout 120s make -C /opt/sming/Sming/Libraries/ConfigDB/test \
  SMING_ARCH=Host SMING_RELEASE=0 FULL_COMPONENT_BUILD=ConfigDB \
  -j4 all execute
```

## Possible firmware trigger: overlapping stream lifetimes

The firmware uses a shared transient JSON-RPC database:

- [RpcCodec::renderStream](app/rpccodec.cpp) retains a ConfigDB `ObjectRef` in an
  outgoing JSON-RPC read stream.
- [JsonProcessor::parseRequest](app/jsonprocessor.cpp) opens an updater on
  `rpcCodec().db()` and imports an HTTP command into its command-request fields.

An outgoing WebSocket notification can therefore retain a read snapshot when a
later HTTP command starts updating the same database. This is overlapping object
lifetime, not necessarily execution on two threads at the same instant.
Copy-on-write should support it safely; timing may expose the missing-reference
defect, but timing alone is not the defect.

This is a plausible trigger, not a confirmed trace of the device failure. A
read-only WebSocket observer does not exclude it because the retained snapshot
belongs to the firmware's outgoing stream.

## Scope and outstanding verification

- The regression proves a ConfigDB defect, not that it caused all reported
  firmware symptoms.
- A separate task-dispatch failure was subsequently confirmed, as detailed below.
- A local diagnostic ConfigDB patch and regression test were applied for the
  experiment, then removed when ConfigDB work was deferred. No ConfigDB PR or
  dependency-pin update was made. The reproduction above records the defect.
- Local firmware containing the diagnostic patch was built and flashed.
  End-to-end exerciser verification was interrupted; a device-level fix must not
  be claimed.
- Before attributing the firmware failure to this defect, capture whether the
  failed HTTP import uses a copied store while an outgoing stream retains the
  original. Separately investigate the missing first state change and subscription
  timeout, including task-queue behaviour.

## Separate confirmed task-dispatch failure

Subsequent ESP32-C3 instrumentation reproduced an independent Sming dispatcher
failure during an OTA test. One lwIP mailbox post returned `ERR_MEM`, but the
dispatcher left its "callback scheduled" flag set. Callback processing stopped,
the Sming queue filled to 32 entries, and subsequent callback posts were rejected.
Networking continued responding, while animation updates and the queued OTA
restart could not execute.

Before that failure was retriggered, a reboot restored output, all 42 solid
command/state checks passed, and the user confirmed physical GREEN output.
Fade-duration checks still had failures and are not resolved by this observation.

A local Sming change retries a rejected wakeup using a one-shot ESP timer rather
than leaving the flag permanently latched. With that change, another OTA to
published `V5.1.0-14-develop` exercised wakeup retries and successfully rebooted
from `rom0` into `rom1`, which reported the published version. This establishes
the dispatcher failure and its OTA impact; it does not negate the independently
reproduced, deferred ConfigDB defect.

The final local build has no temporary queue instrumentation and uses unmodified
ConfigDB. All 42 solid command/state checks passed again on that build, and a
deferred HTTP restart completed successfully. The follow-up core audit added
persistent native actual-source tests in Sming's ESP32 component. They cover 14
deterministic scenarios, one million seeded scheduling steps, and four concurrent
producers in both debug and `NDEBUG` builds. The original dispatcher fails the
accepted-callback delivery regression; the revised dispatcher passes.

An additional OTA attempt using a laptop-hosted image did not establish a
connection to the local image server. That attempt was stopped using the working
HTTP restart path; it is not counted as a successful OTA test. The controller was
left running the local dispatcher fix in `rom1`, with ConfigDB unmodified.

The audit also found that the initial fix's `ESP_ERROR_CHECK` calls would not
enforce failure handling under Sming's release `NDEBUG` setting. Explicit abort
paths replaced them, with an internal-RAM diagnostic string on the ISR path.
That revision builds for ESP32-C3 and passes the native debug/release tests, but
has not been flashed during the follow-up audit. Native concurrency tests are
not equivalent to dual-core target testing or cache-disabled ISR testing; those
remain prerequisites for broad upstream confidence.
