import json
import unittest
from pathlib import Path

from jsonschema import Draft7Validator


ROOT = Path(__file__).resolve().parents[2]


class SceneTargetSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = json.loads((ROOT / "app-data.cfgdb").read_text())
        cls.setting = cls.schema["properties"]["scenes"]["items"]["properties"]["settings"]["items"]

    def test_selectors_are_optional_step_fields_not_colour_fields(self):
        properties = self.setting["properties"]
        self.assertNotIn("light", self.setting.get("required", []))
        self.assertNotIn("light_id", self.setting.get("required", []))
        self.assertEqual(properties["light"]["default"], -1)
        self.assertEqual(properties["light_id"]["default"], "")
        self.assertNotIn("properties", properties["color"])

    def test_selector_bounds_and_types(self):
        properties = self.setting["properties"]
        index = Draft7Validator(properties["light"])
        for value in (-1, 0, 1, 65535):
            with self.subTest(valid=value):
                self.assertTrue(index.is_valid(value))
        for value in (-2, 65536, 1.5, "1", True, None):
            with self.subTest(invalid=value):
                self.assertFalse(index.is_valid(value))
        uuid = Draft7Validator(properties["light_id"])
        self.assertTrue(uuid.is_valid(""))
        self.assertTrue(uuid.is_valid("light-uuid"))
        self.assertFalse(uuid.is_valid(1))

    def test_generated_projection_preserves_the_step_contract(self):
        projection = json.loads((ROOT / "schema/app-data.schema.json").read_text())
        projected = projection["properties"]["scenes"]["items"]["properties"]["settings"]["items"]
        for name in ("light", "light_id"):
            self.assertEqual(projected["properties"][name], self.setting["properties"][name])


if __name__ == "__main__":
    unittest.main()
