#!/usr/bin/env python3
"""覆盖率门槛不能漏文件、接受坏计数或被四舍五入绕过。"""

import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_c_coverage", ROOT / "tools/check_c_coverage.py")
assert SPEC and SPEC.loader
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


class CoverageGateTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="linx-coverage-check-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "main"
        self.source.mkdir()
        for name in ("main.c", "screen.c"):
            (self.source / name).write_text("void function(void) {}\n")
        self.report = self.root / "coverage.json"
        self.payload = {
            "type": "llvm.coverage.json.export", "version": "2.0.1",
            "data": [{"files": [self.file(name) for name in ("main.c", "screen.c")]}],
        }

    def file(self, name):
        return {"filename": str(self.source / name), "summary": {
            "lines": {"count": 100, "covered": 95},
            "branches": {"count": 20, "covered": 18},
            "functions": {"count": 10, "covered": 10},
        }}

    def evaluate(self):
        return CHECK.evaluate(self.payload, self.source)

    def test_full_inventory_and_exact_threshold_pass(self):
        result = self.evaluate()
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["totals"]["branches"], {"count": 40, "covered": 36, "percent": 90.0})
        self.assertEqual(set(result["files"]), {"main.c", "screen.c"})

    def test_missing_source_and_new_unmeasured_source_fail(self):
        self.payload["data"][0]["files"].pop()
        self.assertTrue(any("screen.c" in error for error in self.evaluate()["errors"]))
        (self.source / "new.c").write_text("void new_function(void) {}\n")
        self.assertTrue(any("new.c" in error for error in self.evaluate()["errors"]))

    def test_device_entry_is_never_omitted(self):
        self.payload["data"][0]["files"].pop(0)
        self.assertTrue(any("main.c" in error for error in self.evaluate()["errors"]))

    def test_nested_sources_are_required_and_same_basenames_stay_distinct(self):
        (self.source / "nested").mkdir()
        (self.source / "nested/screen.c").write_text("void nested_function(void) {}\n")
        self.assertTrue(any("nested/screen.c" in error for error in self.evaluate()["errors"]))
        self.payload["data"][0]["files"].append(self.file("nested/screen.c"))
        result = self.evaluate()
        self.assertEqual(result["errors"], [])
        self.assertEqual(set(result["files"]), {"main.c", "screen.c", "nested/screen.c"})

    def test_each_file_and_metric_must_meet_threshold(self):
        for metric in ("lines", "branches", "functions"):
            with self.subTest(metric=metric):
                payload = copy.deepcopy(self.payload)
                payload["data"][0]["files"][0]["summary"][metric] = {"count": 100, "covered": 89}
                result = CHECK.evaluate(payload, self.source)
                self.assertTrue(any("main.c" in error and metric in error for error in result["errors"]))

    def test_rounding_cannot_turn_failure_into_pass(self):
        self.payload["data"][0]["files"][0]["summary"]["lines"] = {"count": 100000, "covered": 89999}
        self.assertTrue(self.evaluate()["errors"])

    def test_generated_and_upstream_data_do_not_raise_application_coverage(self):
        before = self.evaluate()
        extra = self.file("../assets/generated.c")
        extra["summary"]["lines"] = {"count": 100000, "covered": 100000}
        self.payload["data"][0]["files"].append(extra)
        self.assertEqual(self.evaluate(), before)

    def test_same_basename_from_other_checkout_is_not_accepted(self):
        self.payload["data"][0]["files"][0]["filename"] = str(self.root / "elsewhere/main.c")
        self.assertTrue(any("main.c" in error for error in self.evaluate()["errors"]))

    def test_duplicate_source_fails(self):
        self.payload["data"][0]["files"].append(self.file("main.c"))
        with self.assertRaisesRegex(ValueError, "重复"):
            self.evaluate()

    def test_malformed_file_metadata_fails(self):
        for field, value in (("filename", None), ("summary", None),
                             ("summary", {"lines": None})):
            with self.subTest(field=field, value=value):
                payload = copy.deepcopy(self.payload)
                payload["data"][0]["files"][0][field] = value
                with self.assertRaises(ValueError):
                    CHECK.evaluate(payload, self.source)

    def test_invalid_schema_and_empty_inventory_fail(self):
        for payload in ({}, [], {"type": "other"}, {"type": "llvm.coverage.json.export", "data": []}):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                CHECK.evaluate(payload, self.source)
        for path in self.source.glob("*.c"):
            path.unlink()
        with self.assertRaisesRegex(ValueError, "源码"):
            self.evaluate()

    def test_empty_executable_file_or_bad_counters_fail(self):
        for metric, value in (
            ("lines", {"count": 0, "covered": 0}),
            ("lines", {"count": 10, "covered": 11}),
            ("lines", {"count": 10, "covered": -1}),
            ("lines", {"count": True, "covered": 1}),
            ("branches", {"count": 10.0, "covered": 10}),
            ("functions", {"count": 10}),
        ):
            with self.subTest(value=value):
                payload = copy.deepcopy(self.payload)
                payload["data"][0]["files"][0]["summary"][metric] = value
                with self.assertRaises(ValueError):
                    CHECK.evaluate(payload, self.source)

    def test_straight_line_file_can_have_no_branches(self):
        self.payload["data"][0]["files"][0]["summary"]["branches"] = {"count": 0, "covered": 0}
        self.assertEqual(self.evaluate()["errors"], [])

    def test_cli_writes_summary_and_returns_failure_for_low_coverage(self):
        output = self.root / "summary.json"
        self.report.write_text(json.dumps(self.payload))
        with patch.object(CHECK, "ROOT", self.root), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(CHECK.main([str(self.report), "--summary", str(output)]), 0)
            self.assertEqual(json.loads(output.read_text())["errors"], [])
            self.payload["data"][0]["files"][0]["summary"]["lines"]["covered"] = 1
            self.report.write_text(json.dumps(self.payload))
            self.assertEqual(CHECK.main([str(self.report), "--summary", str(output)]), 1)
        self.assertTrue(json.loads(output.read_text())["errors"])

    def test_cli_reports_missing_or_invalid_json(self):
        with patch.object(CHECK, "ROOT", self.root), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(CHECK.main([str(self.report)]), 1)
            self.report.write_text("not-json")
            self.assertEqual(CHECK.main([str(self.report)]), 1)


if __name__ == "__main__":
    unittest.main()
