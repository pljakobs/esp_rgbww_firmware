# Lightinator API Schema Bundle

This bundle is inferred from the current firmware and `esp_rgb_webapp2` implementations. It is marked `inferred` until shared contract fixtures pass against both sides.

## Authoritative Inputs

- `app-config.cfgdb` defines the persistent `/config` object.
- `app-data.cfgdb` defines the persistent `/data` object.
- `pins.cfgdb` defines the separate SoC GPIO capability ConfigDB.
- `defs.cfgdb` defines reusable ConfigDB values.
- `json-rpc-api.cfgdb` defines the transient HTTP payload, JSON-RPC envelope, result, and server-event ConfigDB classes.
- `api.json` maps HTTP operations and JSON-RPC methods/events to payload definitions.
- `openapi.json` defines HTTP paths, methods, query parameters, authentication, statuses, and payload references.
- `hardware-catalog.json.gz` is the versioned board/template catalog served by `GET /catalog`; `soc-pins.json` seeds the firmware-only pin capability DB.

`AppConfig`, `AppData`, and the transient `Api` workspace are separate ConfigDB databases. The API schema references the two persistent cfgdb roots; it does not copy their properties into `json-rpc-api.cfgdb` and does not generate one combined database.

Do not add HTTP retry, controller selection, authentication prompting, or WebSocket lifecycle policy to generated clients. Those remain responsibilities of the frontend transport services.

## Generated Artifacts

Run:

```bash
node tools/generate-api-schemas.mjs
```

This writes:

- `json-rpc-api.schema.json`
- `schema/app-config.schema.json`
- `schema/app-data.schema.json`
- `api-version.json`
- `openapi.generated.json` and its deterministic gzip copy `openapi.generated.json.gz`

The generated OpenAPI 3.1 document combines HTTP metadata from `openapi.json`
with payload projections from the ConfigDB sources. All schema references are
local to the document. HTTP paths, query parameters, and authentication cannot
be inferred from ConfigDB alone and remain maintained in `openapi.json`.

Firmware serves `GET /api/docs` (also `/api/docs/`) and `GET /api/openapi.json`,
using the same conditional Basic authentication as the other API endpoints.
Both support HEAD. The specification is streamed gzip-compressed from flash;
it does not depend on a webapp installation. The docs page loads pinned Swagger
UI 5.31.0 assets from jsDelivr, so viewing the UI requires internet access.
The specification and Try it out requests stay on the controller's origin;
no external validator is used and authorization is not persisted in storage.

Workflow Validation regenerates, tests, and lints the OpenAPI document, checks
for stale committed outputs, and uploads an `api-documentation` artifact. Its
HTML page can also load the adjacent specification for browsing when hosted
together. Use the controller's `/api/docs` page for same-origin Try it out.

These files are read-only projections of the cfgdb sources for tools which cannot load the `.cfgdb` extension or ConfigDB-specific keywords. They are never firmware database inputs. The normalizer converts ConfigDB `$defs` and cross-file references into Draft-07 `definitions`, removes ConfigDB-only generator keywords, and embeds shared definitions where required. `api-version.json` hashes both authoritative cfgdb sources and their projections so source/projection drift is detectable.

`make configdb-rebuild` runs `api-schema-rebuild` first, then generates C++ from the checked-in `.cfgdb` sources. The generated transient API class comes from `json-rpc-api.cfgdb`; the `.schema.json` file is not used for firmware code generation.

Generation is deterministic. Frontend CI should regenerate these files and fail when the working tree changes.

## Validation

```bash
node tools/generate-api-schemas.mjs
python -c 'import json; from jsonschema import Draft7Validator; files=["api-manifest.schema.json","json-rpc-api.cfgdb","json-rpc-api.schema.json","schema/app-config.schema.json","schema/app-data.schema.json"]; schemas=[json.load(open(path)) for path in files]; [Draft7Validator.check_schema(schema) for schema in schemas]; Draft7Validator(schemas[0]).validate(json.load(open("api.json")))'
node --test .github/tests/test_openapi.mjs
npx --yes @redocly/cli@2.12.0 lint openapi.generated.json --extends=minimal
```

Warnings about unused definitions are expected because the normalized ConfigDB schemas intentionally contain the complete shared definition set.

## Frontend Inputs

The frontend generator consumes:

- `api.json` for JSON-RPC method/result/event associations.
- `openapi.json` for HTTP operations.
- `json-rpc-api.cfgdb` as the authoritative transient API schema.
- `json-rpc-api.schema.json` only as its generated browser-compatible projection for transient payload validators.
- `app-config.cfgdb` and `app-data.cfgdb` as the authoritative persistent database schemas.
- `schema/app-config.schema.json` and `schema/app-data.schema.json` only as generated browser-compatible projections of those cfgdb schemas.
- `api-version.json` for compatibility and cache checks.

Generate strict Ajv standalone validators with coercion, defaults, and property removal disabled. UI normalization remains handwritten and runs before outbound validation; inbound firmware data is never normalized into validity.

## Inferred Compatibility

- JSON-RPC `color` is overloaded: empty params read color state, while non-empty params execute a color command. `getColor` is the unambiguous read alias.
- HTTP light operations accept `?light=N`; HTTP command JSON, WebSocket and MQTT JSON accept `"light": N`. If both HTTP selectors are present they must agree. Omitted selectors resolve only the unique configured `is_default` light.
- `hsv` carries HSVCT for every type; each profile consumes only its meaningful components. Raw writes use named `r/g/b/ww/cw` fields for RGB-family types and a pin-ordered array for `WWCW`, `WW`, `CW` and `SINGLE`. Missing array entries and `null` leave channels unchanged; `0` turns a channel off.
- Raw readback/events use the same direct object-or-array shape. ConfigDB's internal `named`/`indexed` discriminator is removed at the wire boundary.
- The firmware accepts aliases such as `getInfo`, `getNetworks`, `on`, and `off`; generated clients should emit canonical names.
- The frontend currently probes JSON-RPC `hosts`, but the WebSocket handler cannot send the stream response and returns method-not-found. The frontend then falls back to `GET /hosts`; `hosts` is therefore not listed as a JSON-RPC method.
- JSON-RPC request IDs generated by the frontend are integers. Firmware preserves string IDs for compatibility, but string IDs are outside the generated canonical contract.
- Color command values are numeric in the generated canonical contract. Current firmware also accepts legacy relative-value strings; those remain a compatibility parser feature because ConfigDB cannot generate scalar unions.
- HTTP Basic authentication is conditional on `security.api_secured`. JSON-RPC uses a SHA-256 challenge response and returns error `-32001` when authentication is required or fails.
- Firmware currently maps unsupported HTTP methods to status 400 rather than 405.
- `/update` is unavailable on Host builds.

Before changing status from `inferred` to `stable`, capture accepted and rejected payload fixtures from firmware integration tests and run them through the generated frontend validators.