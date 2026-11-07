import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { gunzipSync } from "node:zlib";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (path) => readFileSync(resolve(root, path));
const spec = JSON.parse(read("openapi.generated.json"));

test("OpenAPI is self-contained and targets the serving controller", () => {
  assert.equal(spec.openapi, "3.1.0");
  assert.deepEqual(spec.servers, [{ url: "/" }]);
  function visit(value) {
    if (!value || typeof value !== "object") return;
    if (value.$ref) {
      assert.ok(value.$ref.startsWith("#/"), value.$ref);
      const target = value.$ref.slice(2).split("/").reduce(
        (node, key) => node?.[key.replace(/~1/g, "/").replace(/~0/g, "~")], spec,
      );
      assert.notEqual(target, undefined, value.$ref);
    }
    assert.equal(Array.isArray(value.items), false);
    for (const child of Object.values(value)) visit(child);
  }
  visit(spec);
  for (const [name, path] of Object.entries({
    AppConfig: "schema/app-config.schema.json",
    AppData: "schema/app-data.schema.json",
    Wire: "json-rpc-api.schema.json",
  })) {
    const projection = JSON.parse(read(path));
    assert.deepEqual(Object.keys(spec.components.schemas[name].properties), Object.keys(projection.properties));
    assert.deepEqual(Object.keys(spec.components.schemas[name].definitions), Object.keys(projection.definitions));
  }
});

test("firmware gzip matches the validated document and generation is deterministic", () => {
  const paths = ["openapi.generated.json", "openapi.generated.json.gz", "api-version.json"];
  const before = paths.map(read);
  assert.deepEqual(gunzipSync(before[1]), before[0]);
  const generated = spawnSync(process.execPath, ["tools/generate-api-schemas.mjs"], { cwd: root, encoding: "utf8" });
  assert.equal(generated.status, 0, generated.stderr);
  paths.forEach((path, index) => assert.deepEqual(read(path), before[index], path));
});

test("docs use pinned external Swagger assets without an external validator", () => {
  const html = read("api-docs.html").toString();
  assert.match(html, /swagger-ui-dist@5\.31\.0\/swagger-ui\.css/);
  assert.match(html, /swagger-ui-dist@5\.31\.0\/swagger-ui-bundle\.js/);
  assert.match(html, /\/api\/openapi\.json/);
  assert.match(html, /validatorUrl: null/);
  assert.match(html, /persistAuthorization: false/);
});