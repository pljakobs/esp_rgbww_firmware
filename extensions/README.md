# Extension protocol schemas

Each included extension may provide `extensions/<name>/protocol.json`: an array
of complete ConfigDB `oneOf` variants. The build appends them to the root
`oneOf` in `jsonrpc.cfgdb` before ConfigDB preprocessing and code generation.
Entries require a unique `title` (or `$ref`); names must not collide after
ConfigDB identifier normalization. Fragments are processed in path order.

Extensions may also provide `request-params.json`, an object mapping inbound
WebSocket parameter names to ConfigDB property schemas. Its entries are merged
into the generated copy of `params.cfgdb`'s `ws-request-params` object. Identical
definitions may be shared; conflicting names or normalized identifiers are
rejected. References must resolve in the context of `params.cfgdb` or another
database supplied to dbgen. The source schema remains unchanged.

Variants must be self-contained or refer to schemas already passed to dbgen.
An optional `storage.cfgdb` in the same folder generates a separate ConfigDB
class named `Extension<name>`. Extension directory names containing storage
schemas must use lowercase letters, digits or underscores and start with a
letter. Removing an extension removes its generated C++ storage types; it does
not delete any on-device persistent settings.

General automatic discovery of extension code, runtime registry entries,
configuration APIs, and generated public API documentation is not implemented
yet. The AS7341 is compiled when its extension directory is present and
provides a presence-gated measurement API described in
[`as7341/README.md`](as7341/README.md). The SSD1306 and INA219 extension
schema fragments do not yet enable those devices.

The expanded schema is generated at `out/extension-schema/jsonrpc.cfgdb`.
`EXTENSION_SCHEMA_DIR` can select an alternative extension directory. If the
directory is absent, only the core schema is used. To remove an extension,
remove its directory; the next build regenerates the protocol schema and
excludes its runtime code.
