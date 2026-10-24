#!/usr/bin/env python3
"""本项目文档、安全与 CI 检查的隔离回归。"""

from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_repo", ROOT / "tools/check_repo.py")
assert SPEC and SPEC.loader
CHECKS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKS)


class ProjectChecksTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="linx-project-checks-")
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name).resolve()
        subprocess.run(["git", "init", "--quiet", str(self.repo)], check=True)
        self.root = self.repo / "firmware"
        for name in CHECKS.REQUIRED_FILES:
            self.write(name, "# 中文项目文件\n")
        (self.repo / ".gitignore").write_text("firmware/build/\n", encoding="utf-8")

    def write(self, name: str, content: str) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def workflow(self, content: str) -> None:
        path = self.repo / ".github/workflows/firmware.yml"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def test_own_chinese_project_needs_no_copied_upstream_framework(self) -> None:
        self.assertEqual(CHECKS.check_project(self.root), [])
        self.assertFalse((self.root / "skills").exists())
        self.assertFalse((self.root / ".github").exists())

    def test_missing_build_input_fails(self) -> None:
        (self.root / "main/main.c").unlink()
        self.assertTrue(any("main/main.c" in error for error in CHECKS.check_project(self.root)))

    def test_dependency_lock_must_remain_visible_to_git(self) -> None:
        self.write(".gitignore", "dependencies.lock\n")
        self.assertTrue(any("dependencies.lock" in error for error in CHECKS.check_project(self.root)))

    def test_untracked_sources_are_scanned_but_caches_and_parent_files_are_not(self) -> None:
        extra = self.write("main/extra.c", "// 本项目扩展\n")
        self.write("build/generated.md", "# Generated\n")
        (self.repo / "outside.md").write_text("# Other project\n", encoding="utf-8")
        files = CHECKS.git_files(self.root)
        self.assertIn(extra, files)
        self.assertNotIn(self.root / "build/generated.md", files)
        self.assertTrue(all(path.is_relative_to(self.root) for path in files))
        self.assertEqual(CHECKS.check_project(self.root), [])

    def test_local_links_resolve_relative_to_document_or_parent_git_root(self) -> None:
        self.write("README.md", "# 中文说明\n[许可](LICENSE) [指南](/firmware/docs/指南.md)\n")
        guide = self.write("docs/指南.md", "# 中文指南\n[返回](../README.md)\n")
        self.assertEqual(CHECKS.check_project(self.root), [])
        guide.unlink()
        self.assertTrue(any("链接不存在" in error for error in CHECKS.check_project(self.root)))

    def test_remote_and_fragment_links_need_no_local_file(self) -> None:
        self.write("README.md", "# 中文说明\n[外部](https://example.org/guide) [段落](#说明)\n")
        self.assertEqual(CHECKS.check_project(self.root), [])

    def test_chinese_in_code_only_does_not_replace_chinese_prose(self) -> None:
        self.write("README.md", "# Guide\n\n```text\n中文示例\n```\n")
        self.assertTrue(any("中文正文" in error for error in CHECKS.check_project(self.root)))

    def test_sensitive_values_are_flagged_without_echoing_them(self) -> None:
        samples = (
            "ghp_" + "x" * 24,
            "AKIA" + "0" * 16,
            "-----BEGIN " + "PRIVATE KEY-----",
            "https://ai-passport.folotoy.cn/trae/" + "?s=synthetic&k=synthetic",
        )
        for sample in samples:
            with self.subTest(sample_type=sample[:4]):
                self.write("main/example.c", sample)
                errors = CHECKS.check_project(self.root)
                self.assertTrue(any("敏感" in error for error in errors))
                self.assertTrue(all(sample not in error for error in errors))

    def test_merge_conflicts_fail(self) -> None:
        self.write("main/example.c", "<" * 7 + " HEAD\n")
        self.assertTrue(any("冲突" in error for error in CHECKS.check_project(self.root)))

    def test_parent_firmware_workflow_requires_full_commit(self) -> None:
        self.workflow("steps:\n  - uses: actions/checkout@" + "a" * 40 + "\n")
        self.assertEqual(CHECKS.check_project(self.root), [])
        self.workflow("steps:\n  - uses: actions/checkout@main\n")
        self.assertTrue(any("固定提交" in error for error in CHECKS.check_project(self.root)))

    def test_local_and_digest_pinned_actions(self) -> None:
        self.workflow("steps:\n  - uses: ./local-action\n  - uses: docker://image@sha256:" + "a" * 64 + "\n")
        self.assertEqual(CHECKS.check_project(self.root), [])
        self.workflow("steps:\n  - uses: docker://image:latest\n")
        self.assertTrue(any("固定提交" in error for error in CHECKS.check_project(self.root)))

    def test_cli_exit_codes_and_chinese_output(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(CHECKS.main(self.root), 0)
        self.assertIn("通过", output.getvalue())
        (self.root / "LICENSE").unlink()
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            self.assertEqual(CHECKS.main(self.root), 1)
        self.assertIn("LICENSE", errors.getvalue())

    def test_git_failure_is_reported_as_a_failed_check(self) -> None:
        with patch.object(CHECKS, "git_files", side_effect=subprocess.CalledProcessError(1, "git")):
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(CHECKS.main(self.root), 1)


if __name__ == "__main__":
    unittest.main()
