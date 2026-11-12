"""Verify optional controller-local preset metadata and its API projection."""

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class PresetSchemaTests(unittest.TestCase):
    def test_metadata_is_optional_and_projection_matches(self):
        source = json.loads((ROOT / "app-data.cfgdb").read_text())
        projection = json.loads((ROOT / "schema/app-data.schema.json").read_text())
        preset = source["properties"]["presets"]["items"]
        projected = projection["properties"]["presets"]["items"]
        for field in ("light_favorites", "raw_channels", "light_type"):
            self.assertNotIn(field, preset.get("required", []))
            self.assertEqual(preset["properties"][field], projected["properties"][field])

    def test_overrides_include_an_explicit_false_and_stable_uuid(self):
        source = json.loads((ROOT / "app-data.cfgdb").read_text())
        override = source["properties"]["presets"]["items"]["properties"]["light_favorites"]["items"]
        self.assertEqual(override["properties"]["light_id"]["type"], "string")
        self.assertEqual(override["properties"]["favorite"], {"type": "boolean", "default": False})
        self.assertEqual(set(override["required"]), {"light_id", "favorite"})

    def test_indexed_raw_range_and_capability(self):
        source = json.loads((ROOT / "app-data.cfgdb").read_text())
        channels = source["properties"]["presets"]["items"]["properties"]["raw_channels"]["items"]
        self.assertEqual(channels, {"type": "integer", "minimum": 0, "maximum": 1023})
        params = json.loads((ROOT / "params.cfgdb").read_text())
        self.assertEqual(
            params["$defs"]["rgbww-info"]["properties"]["preset_light_favorites"],
            {"type": "boolean"},
        )
