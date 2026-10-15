# OTA Operation Guide

This document describes how OTA firmware updates are expected to operate in this project, including:

- normal OTA behavior (current standard path)
- transitional OTA behavior for legacy ESP8266 partition layouts
- risk boundaries for maintainers

## Scope

This guide covers the OTA logic implemented in `app/otaupdate.cpp` and related Sming OTA network updater components.

## Definitions

- OTA: Over-The-Air firmware update.
- Running partition: The firmware partition currently executing.
- Next boot partition: The partition selected as OTA target and later used on reboot.
- Transitional layout: Legacy ESP8266 devices using older partition/filesystem schemes that may require migration.

## ESP8266 Layout Overview (ASCII)

The migration discussion is easier to follow with a side-by-side map.

```text
Legacy layout (pre-migration)                  Modern/transitional layout

0x00000000  +-------------------------+        0x00000000  +-------------------------+
            | Partition table         |                    | Partition table         |
0x00002000  +-------------------------+        0x00002000  +-------------------------+
            | rom0                    |                    | rom0                    |
            |                         |                    |                         |
0x000FA000  +-------------------------+        0x000FA000  +-------------------------+
0x00100000  +-------------------------+        0x00100000  +-------------------------+
            | spiffs0                 |                    | lfs0 (expanded)         |
            | (~768KB)                |                    | (~1000KB)               |
0x001C0000  +-------------------------+        0x001FA000  +-------------------------+
0x00202000  +-------------------------+        0x00202000  +-------------------------+
            | rom1                    |                    | rom1                    |
            |                         |                    |                         |
0x002FA000  +-------------------------+        0x002FA000  +-------------------------+
0x00300000  +-------------------------+        0x00300000  +-------------------------+
            | spiffs1                 |                    | lfs1 (expanded)         |
            | (~768KB)                |                    | (~1000KB)               |
0x003C0000  +-------------------------+        0x003FA000  +-------------------------+
0x003FFFFF  +-------------------------+        0x003FFFFF  +-------------------------+
```

Notes:

- Address values above reflect the migration model currently documented in firmware comments.
- This layout discussion primarily applies to ESP8266 transitional upgrades.

## Normal OTA Process (Current Standard Path)

The normal OTA process is intended for devices already on the modern layout.

1. Request arrives with firmware URL.
2. OTA updater object is created (`Ota::Network::HttpUpgrader`).
3. Target partition is selected using `getNextBootPartition()`.
4. Safety gate rejects update if target equals running partition.
5. OTA item is queued (`addItem(url, partition)`).
6. Completion callback is attached (`setCallback(...)`).
7. Watchdog timer is armed as recovery guard.
8. OTA status is broadcast to websocket clients (coarse phases).
9. `HttpUpgrader` downloads and writes firmware to target partition.
10. Callback handles success/failure:
   - success: finalize status, set boot partition, reboot
   - failure: abort/reset updater state, broadcast failure, reboot via timed recovery path

## Coarse OTA Status Phases (Websocket)

The current frontend contract is intentionally simple:

- step `1`: Preparing
- step `2`: Downloading
- step `3`: Verifying/Finalizing
- step `4`: Success and rebooting
- step `0`: Failure

Notes:

- Progress within download is not currently available from the documented `HttpUpgrader` API.
- Failure details should be conveyed in message text, not by adding extra step codes.

## Transitional OTA Process (Legacy ESP8266 Migration)

Some older ESP8266 devices may still require migration from legacy partition/filesystem layouts.

High-level transition intent:

1. Detect whether device is on old layout (legacy SPIFFS-based scheme).
2. Rewrite partition table for modern layout where required.
3. Create LittleFS partitions (`lfs0`, `lfs1`) as needed.
4. Preserve essential runtime/config state where feasible.
5. Ensure resulting image boots and remains OTA-updatable.

Important constraints:

- This path is high-risk because it modifies partition metadata and filesystem layout.
- If migration logic regresses, affected field devices may become unrecoverable without physical access.
- Any changes to migration logic must be canary-tested on representative hardware first.

## Boot-Time OTA State Handling

On boot, OTA status is loaded from persistent storage and normalized.

Intended behavior:

- interrupted states (failed/processing from previous run) are recognized and cleared so the device can continue normal operation.
- boot state persistence is for recovery/diagnostics, not live UI progress streaming.

## Recovery Strategy

Recovery currently relies on automatic restart behavior in failure/stall scenarios:

- watchdog-triggered restart if OTA appears hung
- delayed restart after failed OTA attempt to restore network/stack reachability

This is a critical safety mechanism for remote-only devices.

## High-Risk Code Regions (Maintainer Guidance)

The following regions in `app/otaupdate.cpp` should be treated as high-risk and modified only with staged rollout:

- OTA transaction setup/start path
- completion callback success/failure control flow
- watchdog and reboot behavior
- boot partition selection/switch behavior
- boot-time persisted OTA state handling
- ESP8266 partition migration and partition-table writes

## Change Policy

When modifying OTA code:

1. Prefer additive, non-blocking changes to status messaging first.
2. Avoid altering ordering of partition select/write/switch/restart unless strictly required.
3. Test both success and failure/recovery paths on real hardware.
4. Perform canary rollout before broad deployment.

## Out of Scope

- Detailed Sming internals beyond documented updater API.
- UI implementation details outside OTA status contract.
- Device-specific factory recovery procedures.
