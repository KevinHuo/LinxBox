#!/usr/bin/env python3
"""共享 Schema 与固件生成描述的漂移检查。C fixtures 检查由 controller 单测执行。"""
import importlib.util
import json
from pathlib import Path
import subprocess
import unittest
from unittest import mock
import runpy
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("generator", ROOT / "firmware/tools/generate_gateway_contract.py")
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class GatewayContractTest(unittest.TestCase):
    def test_generated_file_current(self):
        subprocess.run(["python3", str(ROOT / "firmware/tools/generate_gateway_contract.py"), "--check"], check=True)

    def test_unknown_keyword_is_error(self):
        for keyword, value in (("maxProperties", 5), ("patternProperties", {}), ("format", "uuid"), ("unevaluatedProperties", False)):
            with self.subTest(keyword=keyword), self.assertRaises(ValueError):
                GENERATOR.generate({"a.json": {"type": "object", keyword: value}})

    def test_unsupported_pattern_and_type(self):
        with self.assertRaises(ValueError):
            GENERATOR.generate({"a.json": {"type": "string", "pattern": ".*"}})
        with self.assertRaises(ValueError):
            GENERATOR.generate({"a.json": {"type": "number"}})
        with self.assertRaises(ValueError):
            GENERATOR.generate({"a.json": {"const": 4}})
        with self.assertRaises(ValueError):
            GENERATOR.generate({"a.json": {"enum": [4]}})
        with self.assertRaises(ValueError):
            GENERATOR.generate({"a.json": {"type": "object", "required": ["missing"]}})

    def test_schema_shapes_fail_explicitly(self):
        for schema in (True, {"type": ["string", "null"]}, {"additionalProperties": {"type": "string"}}):
            with self.subTest(schema=schema), self.assertRaises(ValueError):
                GENERATOR.generate({"a.json": schema})

    def test_main_write_and_check(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "gateway.h"
            with mock.patch.object(GENERATOR, "OUTPUT", output):
                with mock.patch("sys.argv", ["generator", "--check"]), self.assertRaises(SystemExit):
                    GENERATOR.main()
                with mock.patch("sys.argv", ["generator"]):
                    GENERATOR.main()
                self.assertTrue(output.read_text().startswith("/*"))
                with mock.patch("sys.argv", ["generator", "--check"]):
                    GENERATOR.main()
                output.write_text("outdated")
                with mock.patch("sys.argv", ["generator", "--check"]), self.assertRaises(SystemExit):
                    GENERATOR.main()
        with mock.patch("sys.argv", ["generator", "--check"]):
            runpy.run_path(str(ROOT / "firmware/tools/generate_gateway_contract.py"), run_name="__main__")

    def test_formal_content_maximum_is_generated(self):
        schemas = {name: json.loads((ROOT / "contracts" / name).read_text()) for name in
                   ("common.schema.json", "firmware-gateway.schema.json")}
        baseline = GENERATOR.generate(schemas)
        schemas["common.schema.json"]["$defs"]["ContentChunk"]["properties"]["next_offset"]["anyOf"][0]["maximum"] = 65535
        self.assertNotEqual(baseline, GENERATOR.generate(schemas))
        self.assertIn("minimum, maximum", baseline)

    def test_formal_contract_excludes_local_extensions(self):
        common = json.loads((ROOT / "contracts/common.schema.json").read_text())["$defs"]
        firmware = json.loads((ROOT / "contracts/firmware-gateway.schema.json").read_text())["$defs"]
        self.assertNotIn("request.propose", firmware)
        self.assertNotIn("limit", firmware["agent_sessions.list"]["properties"]["payload"]["properties"])
        self.assertNotIn("limit", common["MessagesListInput"]["properties"])
        self.assertNotIn("delivery_confirmed", common["Request"]["properties"])
        self.assertNotIn("last_turn_status", common["AgentSession"]["properties"])
        self.assertNotIn("waiting", common["AgentSession"]["properties"]["status"]["enum"])

    def test_condition_constraints_are_generated(self):
        schemas = {name: json.loads((ROOT / "contracts" / name).read_text()) for name in
                   ("common.schema.json", "firmware-gateway.schema.json")}
        baseline = GENERATOR.generate(schemas)
        schemas["common.schema.json"]["$defs"]["Request"]["allOf"].pop()
        self.assertNotEqual(baseline, GENERATOR.generate(schemas))
        schemas["common.schema.json"]["$defs"]["Id"]["maxLength"] = 129
        self.assertIn("CONTRACT_ID_POINTS 129U", GENERATOR.generate(schemas))


if __name__ == "__main__":
    unittest.main()
