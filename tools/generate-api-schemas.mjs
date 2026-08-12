import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const outputDirectory = join(root, "schema");

const readText = async (name) => readFile(join(root, name), "utf8");
const readJson = async (name) => JSON.parse(await readText(name));
const sha256 = (value) => createHash("sha256").update(value).digest("hex");

function normalize(value) {
  if (Array.isArray(value)) {
    return value.map(normalize);
  }
  if (value === null || typeof value !== "object") {
    return value;
  }

  const result = {};
  for (const [key, child] of Object.entries(value)) {
    if (["store", "alias", "ctype", "@default"].includes(key)) {
      continue;
    }
    if (key === "$defs") {
      result.definitions = normalize(child);
      continue;
    }
    if (key === "$ref" && typeof child === "string") {
      result[key] = child
        .replace(/^defs\/\$defs\//, "#/definitions/")
        .replace(/^#\/\$defs\//, "#/definitions/");
      continue;
    }
    result[key] = normalize(child);
  }
  return result;
}

function stableJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

const definitionsSource = await readJson("defs.cfgdb");
const sharedDefinitions = normalize(definitionsSource.$defs ?? {});

async function generatePersistedSchema(sourceName, outputName, title, database) {
  const source = normalize(await readJson(sourceName));
  const schema = {
    ...source,
    $id: `https://lightinator.de/schemas/${outputName}`,
    title,
    $comment: `Generated projection of ${sourceName}; ${sourceName} remains the authoritative ${database} ConfigDB schema.`,
    "x-configdb-source": sourceName,
    "x-configdb-database": database,
    definitions: {
      ...sharedDefinitions,
      ...(source.definitions ?? {}),
    },
  };
  await writeFile(join(outputDirectory, outputName), stableJson(schema));
  return schema;
}

await mkdir(outputDirectory, { recursive: true });
const config = await generatePersistedSchema(
  "app-config.cfgdb",
  "app-config.schema.json",
  "Lightinator persistent configuration",
  "AppConfig",
);
const data = await generatePersistedSchema(
  "app-data.cfgdb",
  "app-data.schema.json",
  "Lightinator persistent application data",
  "AppData",
);
const wire = await readJson("json-rpc-api.schema.json");
const manifest = await readJson("api.json");
const openapi = await readJson("openapi.json");

const sourceSchemas = [
  { database: "Definitions", source: "defs.cfgdb", persistent: false },
  { database: "AppConfig", source: "app-config.cfgdb", projection: "schema/app-config.schema.json", persistent: true },
  { database: "AppData", source: "app-data.cfgdb", projection: "schema/app-data.schema.json", persistent: true },
];
for (const schema of sourceSchemas) {
  schema.sourceHash = sha256(await readText(schema.source));
  if (schema.projection) {
    schema.projectionHash = sha256(await readText(schema.projection));
  }
}

const digest = createHash("sha256");
for (const source of ["defs.cfgdb", "app-config.cfgdb", "app-data.cfgdb"]) {
  digest.update(await readText(source));
}
for (const artifact of [manifest, wire, openapi]) {
  digest.update(stableJson(artifact));
}

const version = {
  apiVersion: manifest.version,
  status: manifest.status,
  schemaHashAlgorithm: "sha256",
  schemaHash: digest.digest("hex"),
  sourceSchemas,
  artifacts: [
    "api.json",
    "json-rpc-api.schema.json",
    "defs.cfgdb",
    "app-config.cfgdb",
    "app-data.cfgdb",
    "schema/app-config.schema.json",
    "schema/app-data.schema.json",
    "openapi.json",
  ],
};
await writeFile(join(root, "api-version.json"), stableJson(version));