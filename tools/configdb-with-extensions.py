#!/usr/bin/env python3
"""Compose extension message variants before invoking Sming's ConfigDB generator."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


PROJECT_ROOT = Path(__file__).resolve().parent.parent


def identifier(name):
    result = ""
    upper = False
    for character in re.sub(r"[^A-Za-z0-9]+", "_", name):
        if character == "_":
            upper = True
        else:
            result += character.upper() if upper else character
            upper = False
    return result


def variant_name(variant, source):
    if not isinstance(variant, dict):
        raise ValueError(f"{source}: each protocol variant must be an object")
    name = variant.get("title")
    if not name and isinstance(variant.get("$ref"), str):
        name = variant["$ref"].rsplit("/", 1)[-1]
    if not isinstance(name, str) or not name or not identifier(name):
        raise ValueError(f"{source}: each protocol variant needs a nonempty title or $ref")
    return name


def compose(core_path, extensions_dir):
    with core_path.open(encoding="utf-8") as source:
        schema = json.load(source)
    if not isinstance(schema, dict) or not isinstance(schema.get("oneOf"), list):
        raise ValueError(f"{core_path}: root must contain a oneOf array")

    variants = schema["oneOf"]
    names = {}
    for variant in variants:
        name = variant_name(variant, core_path)
        key = identifier(name)
        if key in names:
            raise ValueError(f"{core_path}: duplicate variant {name!r} conflicts with {names[key]}")
        names[key] = str(core_path)

    if not extensions_dir.exists():
        return schema
    if not extensions_dir.is_dir():
        raise ValueError(f"{extensions_dir}: extensions path must be a directory")

    for fragment in sorted(extensions_dir.glob("*/protocol.json")):
        with fragment.open(encoding="utf-8") as source:
            entries = json.load(source)
        if not isinstance(entries, list):
            raise ValueError(f"{fragment}: protocol fragment must be an array of oneOf variants")
        for variant in entries:
            name = variant_name(variant, fragment)
            key = identifier(name)
            if key in names:
                raise ValueError(f"{fragment}: duplicate variant {name!r} conflicts with {names[key]}")
            names[key] = str(fragment)
            variants.append(variant)
    return schema


def compose_request_params(core_path, extensions_dir):
    with core_path.open(encoding="utf-8") as source:
        schema = json.load(source)
    try:
        properties = schema["$defs"]["ws-request-params"]["properties"]
    except (KeyError, TypeError):
        raise ValueError(f"{core_path}: missing ws-request-params properties")
    if not isinstance(properties, dict):
        raise ValueError(f"{core_path}: ws-request-params properties must be an object")

    names = {identifier(name): (name, value) for name, value in properties.items()}
    if not extensions_dir.exists():
        return schema
    for fragment in sorted(extensions_dir.glob("*/request-params.json")):
        with fragment.open(encoding="utf-8") as source:
            additions = json.load(source)
        if not isinstance(additions, dict):
            raise ValueError(f"{fragment}: request parameters must be an object")
        for name, value in additions.items():
            if not isinstance(name, str) or not name or not identifier(name) or not isinstance(value, dict):
                raise ValueError(f"{fragment}: each parameter needs a nonempty name and object schema")
            key = identifier(name)
            if key in names:
                existing_name, existing_value = names[key]
                if name != existing_name or value != existing_value:
                    raise ValueError(f"{fragment}: conflicting request parameter {name!r}")
                continue
            properties[name] = value
            names[key] = (name, value)
    return schema


def write_if_changed(path, schema):
    content = json.dumps(schema, indent=2, ensure_ascii=False) + "\n"
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as output:
            temp_path = Path(output.name)
            output.write(content)
        os.replace(temp_path, path)
    finally:
        if temp_path is not None:
            temp_path.unlink(missing_ok=True)


def storage_schemas(extensions_dir, generated_dir):
    schemas = []
    names = set()
    if not extensions_dir.exists():
        return schemas
    for path in sorted(extensions_dir.glob("*/storage.cfgdb")):
        extension = path.parent.name
        if not re.fullmatch(r"[a-z][a-z0-9_]*", extension):
            raise ValueError(f"{path}: extension directory must use lowercase letters, digits or underscores")
        name = f"extension_{extension}"
        normalized = identifier(name)
        if normalized in names:
            raise ValueError(f"{path}: conflicting storage database name")
        names.add(normalized)
        with path.open(encoding="utf-8") as source:
            contents = json.load(source)
        target = generated_dir / f"{name}.cfgdb"
        write_if_changed(target, contents)
        schemas.append((name, target, contents))
    return schemas


def remove_old_storage_outputs(outdir, names):
    manifest = outdir / "schema" / "extension-storage-manifest.json"
    if manifest.exists():
        previous = json.loads(manifest.read_text(encoding="utf-8"))
        if not isinstance(previous, list) or not all(
            isinstance(name, str) and re.fullmatch(r"extension_[a-z][a-z0-9_]*", name) for name in previous
        ):
            raise ValueError(f"{manifest}: invalid extension storage manifest")
        for name in set(previous) - set(names):
            for path in (outdir / f"{name}.h", outdir / f"{name}.cpp", outdir / "schema" / f"{name}.json"):
                path.unlink(missing_ok=True)
    write_if_changed(manifest, names)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--preprocess", action="store_true")
    parser.add_argument("cfgfiles", nargs="+")
    args = parser.parse_args()

    source = PROJECT_ROOT / "jsonrpc.cfgdb"
    matching = [path for path in args.cfgfiles if Path(path).resolve() == source]
    if len(matching) != 1:
        raise ValueError("ConfigDB input must contain the project's jsonrpc.cfgdb exactly once")
    params_source = PROJECT_ROOT / "params.cfgdb"
    params_matching = [path for path in args.cfgfiles if Path(path).resolve() == params_source]
    if len(params_matching) != 1:
        raise ValueError("ConfigDB input must contain the project's params.cfgdb exactly once")

    extensions = Path(os.environ.get("EXTENSION_SCHEMA_DIR", PROJECT_ROOT / "extensions"))
    generated_dir = PROJECT_ROOT / "out" / "extension-schema"
    generated = generated_dir / "jsonrpc.cfgdb"
    generated_params = generated_dir / "params.cfgdb"
    storage = storage_schemas(extensions, generated_dir)
    expanded = compose(source, extensions)
    write_if_changed(generated_params, compose_request_params(params_source, extensions))
    fingerprint = hashlib.sha256(
        json.dumps([(name, contents) for name, _, contents in storage], sort_keys=True).encode("utf-8")
    ).hexdigest()
    expanded["$comment"] = f'{expanded.get("$comment", "")} Extension storage schemas: {fingerprint}'
    write_if_changed(generated, expanded)
    remove_old_storage_outputs(Path(args.outdir), [name for name, _, _ in storage])
    sming_home = os.environ.get("SMING_HOME")
    if not sming_home:
        raise ValueError("SMING_HOME is not set")
    generator = Path(sming_home) / "Libraries" / "ConfigDB" / "tools" / "dbgen.py"
    replacements = {matching[0]: str(generated), params_matching[0]: str(generated_params)}
    files = [replacements.get(path, path) for path in args.cfgfiles]
    files.extend(str(path) for _, path, _ in storage)
    command = [sys.executable, str(generator), "--outdir", args.outdir]
    if args.preprocess:
        command.append("--preprocess")
    return subprocess.run(command + files, check=False).returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Extension schema composition failed: {error}", file=sys.stderr)
        sys.exit(1)
