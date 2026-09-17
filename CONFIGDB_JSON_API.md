# ConfigDB JSON API Reference

## Generated Database

`json-rpc-api.cfgdb` is the authoritative transient API schema. Running
`make configdb-rebuild` generates:

- `out/ConfigDB/json-rpc-api.h`
- `out/ConfigDB/json-rpc-api.cpp`
- `out/ConfigDB/schema/json-rpc-api.json`

The generated database class is `JsonRpcApi`. Its schema has `store: false`, so
it provides typed temporary request, response, event, and HTTP payload
workspaces without creating another persistent database. `AppConfig` and
`AppData` remain separate persistent databases.

`make configdb-rebuild` first runs `api-schema-rebuild`. That target derives
`json-rpc-api.schema.json` for OpenAPI and browser tooling, then ConfigDB
generates C++ from the checked-in `.cfgdb` source. Never edit the generated
projection or files under `out/ConfigDB` directly.

## Root Workspaces

| Generated type | Schema definition | Purpose |
|---|---|---|
| `JsonRpcApi::Root::Request` | `jsonRpcRequest` | Incoming JSON-RPC envelope |
| `JsonRpcApi::Root::Response` | `jsonRpcResponse` | Success/error response union |
| `JsonRpcApi::Root::Event` | `eventEnvelope` | Outgoing server event envelope |
| `JsonRpcApi::Root::Color` | `colorState` | Raw and HSV color response |
| `JsonRpcApi::Root::ColorCommand` | `colorParams` | Single or batched color command workspace |
| `JsonRpcApi::Root::AnimationCommand` | `animationParams` | Animation command parameters |
| `JsonRpcApi::Root::Info` | `infoResult` | Version 1/version 2 info union |
| `JsonRpcApi::Root::Networks` | `networksResult` | Wi-Fi scan state and entries |
| `JsonRpcApi::Root::Connection` | `connectionResult` | Wi-Fi connection result |
| `JsonRpcApi::Root::HostList` | `hostsResult` | Discovered controller list |
| `JsonRpcApi::Root::ApiResult` | `apiResult` | HTTP success/error union |

Each workspace exposes a read-only object, `update()` for mutation/import, and
`createExportStream()` for serialization. Response, info, and result union
updaters expose `as<Type>()` and `to<Type>()`; union readers expose `getTag()`
and `as<Type>()`. Color parameters use one object workspace because ConfigDB
cannot currently generate the recursive single-command/command-list union.

```cpp
#include "json-rpc-api.h"

JsonRpcApi api;
JsonRpcApi::Root::Request request(api);

ConfigDB::Status status;
if(auto update = request.update()) {
    status = update.importFromStream(ConfigDB::Json::format, inputStream);
}

if(status) {
    auto method = request.getMethod();
    auto id = request.getId();
    // Dispatch using generated accessors and typed payload workspaces.
}
```

```cpp
JsonRpcApi::Root::Color color(api);
if(auto update = color.update()) {
    update.raw.setR(red);
    update.raw.setG(green);
    update.hsv.setH(hue);
}

auto output = color.createExportStream(ConfigDB::Json::format);
response.sendDataStream(output.release(), MIME_JSON);
```

The transport owns HTTP status, authentication, framing, and stream lifetime.
ConfigDB owns JSON parsing, schema validation, typed storage, and serialization.

## HTTP Operations

| Method | Path | Operation | Input | Result |
|---|---|---|---|---|
| GET | `/config` | `getConfig` | - | `app-config.cfgdb` root |
| POST | `/config` | `updateConfig` | `app-config.cfgdb` root | `apiSuccess` |
| GET | `/data` | `getData` | - | `app-data.cfgdb` root |
| POST | `/data` | `updateData` | `app-data.cfgdb` root | `apiSuccess` |
| GET | `/info` | `getInfo` | Query parameters | `infoResult` |
| GET | `/color` | `getColor` | - | `colorState` |
| POST | `/color` | `setColor` | `colorParams` | `apiSuccess` |
| GET | `/networks` | `getNetworks` | - | `networksResult` |
| POST | `/scan_networks` | `scanNetworks` | - | `apiSuccess` |
| GET | `/connect` | `getConnection` | - | `connectionResult` |
| POST | `/connect` | `connect` | `connectParams` | `apiSuccess` |
| POST | `/system` | `system` | `systemParams` | `apiSuccess` |
| GET | `/update` | `getUpdate` | - | `updateStatus` |
| POST | `/update` | `startUpdate` | `updateParams` | `apiSuccess` |
| GET | `/ping` | `ping` | - | `pingResult` |
| POST | `/stop` | `stop` | `animationParams` | `apiSuccess` |
| POST | `/skip` | `skip` | `animationParams` | `apiSuccess` |
| POST | `/pause` | `pause` | `animationParams` | `apiSuccess` |
| POST | `/continue` | `continue` | `animationParams` | `apiSuccess` |
| POST | `/blink` | `blink` | `blinkParams` | `apiSuccess` |
| POST | `/toggle` | `toggle` | `animationParams` | `apiSuccess` |
| POST | `/on` | `setOn` | `animationParams` | `apiSuccess` |
| POST | `/off` | `setOff` | `animationParams` | `apiSuccess` |
| GET | `/hosts` | `getHosts` | Query parameters | `hostsResult` |
| GET | `/webapp_status` | `getWebappStatus` | - | `webappOtaStatusParams` |
| GET | `/webapp_check` | `getWebappCheck` | - | `webappOtaStatusParams` |
| POST | `/webapp_check` | `checkWebapp` | - | `webappOtaStatusParams` |

HTTP Basic authentication is conditional on `security.api_secured`. The Host
build does not expose `/update`. Current firmware returns HTTP 400 rather than
405 for unsupported methods.

## JSON-RPC Methods

The WebSocket endpoint is `/ws` and uses JSON-RPC 2.0.

| Method | Aliases | Params | Result |
|---|---|---|---|
| `authenticate` | - | `authenticateParams` | `authenticateResult` |
| `info` | `getInfo` | `infoParams` | `infoResult` |
| `color` | `getColor` for the empty-params read form | `colorParams` | `commandResult` or `colorState` |
| `networks` | `getNetworks` | `emptyParams` | `networksResult` |
| `stop` | - | `animationParams` | `commandResult` |
| `skip` | - | `animationParams` | `commandResult` |
| `pause` | - | `animationParams` | `commandResult` |
| `continue` | - | `animationParams` | `commandResult` |
| `blink` | - | `blinkParams` | `commandResult` |
| `toggle` | - | `animationParams` | `commandResult` |
| `direct` | - | `animationParams` | `commandResult` |
| `setOn` | `on` | `animationParams` | `commandResult` |
| `setOff` | `off` | `animationParams` | `commandResult` |
| `keep_alive` | - | `emptyParams` | `commandResult` |
| `scan_networks` | - | `emptyParams` | `commandResult` |
| `system` | - | `systemParams` | `commandResult` |
| `webapp_check` | - | `emptyParams` | `commandResult` |
| `runtime_info_subscribe` | `subscribe_runtime_info` | `runtimeSubscriptionParams` | `subscriptionResult` |
| `runtime_info_unsubscribe` | `unsubscribe_runtime_info` | `runtimeSubscriptionParams` | `subscriptionResult` |

`authenticate` and `keep_alive` do not require an authenticated session.
JSON-RPC request IDs are integers in the generated canonical contract.

## Server Events

| Method | Params |
|---|---|
| `color_event` | `colorEventParams` |
| `runtime_info` | `runtimeInfoParams` |
| `notification` | `messageEventParams` |
| `webapp_cmd` | `messageEventParams` |
| `ota_status` | `otaStatusParams` |
| `webapp_ota_status` | `webappOtaStatusParams` |
| `clock_slave_status` | `clockSlaveStatusParams` |
| `transition_finished` | `transitionFinishedParams` |
| `keep_alive` | `emptyParams` |

## JSON-RPC Errors

| Code | Meaning |
|---:|---|
| -32700 | Parse error |
| -32600 | Invalid request |
| -32601 | Method not found |
| -32603 | Internal error |
| -32001 | Authentication required or failed |
| -32000 | Command failed |

## Validation and Regeneration

```bash
make api-schema-rebuild
make configdb-rebuild
```

The second command regenerates all ConfigDB databases and currently reports:

```text
CFGDB app-config.cfgdb app-data.cfgdb defs.cfgdb json-rpc-api.cfgdb
Loading "json-rpc-api.cfgdb"
Parsing "json-rpc-api"
```

Use `api.json` for transport-to-schema associations, `openapi.json` for the
HTTP contract, and `json-rpc-api.schema.json` for browser validation. The
`.cfgdb` files remain the only authoritative ConfigDB inputs.