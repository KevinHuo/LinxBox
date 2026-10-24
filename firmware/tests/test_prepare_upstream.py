#!/usr/bin/env python3
"""用隔离的本地 Git 仓库验证固定上游依赖准备流程。"""

from __future__ import annotations

import contextlib
import io
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "prepare_upstream.py"
SPEC = importlib.util.spec_from_file_location("prepare_upstream", SCRIPT)
assert SPEC and SPEC.loader
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)


class PrepareUpstreamTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.project = self.root / "project" / "firmware"
        self.project.mkdir(parents=True)
        self.source = self.root / "source"
        self.source.mkdir()
        self.git(self.source, "init", "--quiet")
        self.git(self.source, "config", "user.name", "依赖测试")
        self.git(self.source, "config", "user.email", "upstream-test@example.invalid")
        for name in (
            "CMakeLists.txt",
            "LICENSE",
            "components/bsp/CMakeLists.txt",
            "main/key_guard.h",
            "tools/validate.sh",
            "tools/verify_firmware.py",
            "tools/archive_firmware.py",
        ):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("固定版本内容\n", encoding="utf-8")
        (self.source / ".gitignore").write_text("build/\n__pycache__/\n", encoding="utf-8")
        self.git(self.source, "add", ".")
        self.git(self.source, "commit", "--quiet", "-m", "上游基线")
        self.commit = self.git(self.source, "rev-parse", "HEAD")
        self.repository = "https://example.invalid/FoloToy/ai-passport.git"
        self.write_metadata()

    @staticmethod
    def git(directory: Path, *args: str) -> str:
        return subprocess.run(
            ["git", "-C", str(directory), *args],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

    def write_metadata(self, **overrides: object) -> None:
        upstream = {"repository": self.repository, "baseline_commit": self.commit}
        upstream.update(overrides)
        (self.project / "upstream.json").write_text(
            json.dumps({"upstream": upstream}), encoding="utf-8"
        )

    @property
    def cache(self) -> Path:
        return self.project.parent / ".local" / "dependencies" / "ai-passport" / self.commit

    def prepare(self) -> Path:
        with contextlib.redirect_stderr(io.StringIO()):
            return PREPARE.prepare_upstream(self.project, str(self.source))

    def test_prepares_exact_commit_with_official_origin(self) -> None:
        (self.source / "main/key_guard.h").write_text("较新的内容\n", encoding="utf-8")
        self.git(self.source, "add", ".")
        self.git(self.source, "commit", "--quiet", "-m", "后续上游修改")
        result = self.prepare()
        self.assertEqual(result, self.cache)
        self.assertTrue(result.is_absolute())
        self.assertFalse(result.is_relative_to(self.project))
        self.assertEqual(self.git(result, "rev-parse", "HEAD"), self.commit)
        self.assertEqual(self.git(result, "rev-parse", "--abbrev-ref", "HEAD"), "HEAD")
        self.assertEqual(self.git(result, "remote", "get-url", "origin"), self.repository)
        self.assertEqual((result / "main/key_guard.h").read_text(), "固定版本内容\n")
        self.assertEqual(self.git(result, "status", "--porcelain"), "")

    def test_reuses_verified_cache_without_source_or_fetch(self) -> None:
        self.prepare()
        shutil.rmtree(self.source)
        with mock.patch.object(PREPARE.subprocess, "run", wraps=subprocess.run) as run:
            self.assertEqual(self.prepare(), self.cache)
        self.assertFalse(any("fetch" in call.args[0] for call in run.call_args_list))

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "增量配置验证需要 CMake 与 Ninja")
    def test_existing_cmake_build_revalidates_metadata_and_preparer_changes(self) -> None:
        self.prepare()
        shutil.copyfile(ROOT / "CMakeLists.txt", self.project / "CMakeLists.txt")
        preparer = self.project / "tools/prepare_upstream.py"
        preparer.parent.mkdir()
        shutil.copyfile(SCRIPT, preparer)
        board = self.project / "hardware/folotoy/ai-passport/board.cmake"
        board.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / "hardware/folotoy/ai-passport/board.cmake", board)
        fake_idf = self.root / "fake-idf"
        include = fake_idf / "tools/cmake/project.cmake"
        include.parent.mkdir(parents=True)
        include.write_text("# 本测试只验证 CMake 的增量配置，不模拟固件编译。\n", encoding="utf-8")
        environment = {**os.environ, "IDF_PATH": str(fake_idf)}
        build = self.root / "cmake-build"
        subprocess.run(
            ["cmake", "-S", str(self.project), "-B", str(build), "-G", "Ninja"],
            check=True, capture_output=True, text=True, env=environment,
        )
        self.write_metadata(baseline_commit="invalid-ref")
        result = subprocess.run(
            ["cmake", "--build", str(build)], capture_output=True, text=True, env=environment,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("40 位", result.stdout + result.stderr)
        self.write_metadata()
        subprocess.run(
            ["cmake", "--build", str(build)], check=True,
            capture_output=True, text=True, env=environment,
        )
        preparer.write_text("raise SystemExit('准备逻辑已更改')\n", encoding="utf-8")
        result = subprocess.run(
            ["cmake", "--build", str(build)], capture_output=True, text=True, env=environment,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("准备逻辑已更改", result.stdout + result.stderr)

    def test_ignored_build_outputs_do_not_dirty_cache(self) -> None:
        self.prepare()
        (self.cache / "build").mkdir()
        (self.cache / "build/output.bin").write_bytes(b"\0")
        self.assertEqual(self.prepare(), self.cache)

    def test_rejects_modified_tracked_file_without_overwriting_it(self) -> None:
        self.prepare()
        changed = self.cache / "main/key_guard.h"
        changed.write_text("保留我的修改\n", encoding="utf-8")
        with self.assertRaisesRegex(PREPARE.UpstreamError, "修改|干净"):
            self.prepare()
        self.assertEqual(changed.read_text(), "保留我的修改\n")

    def test_rejects_staged_changes(self) -> None:
        self.prepare()
        (self.cache / "main/key_guard.h").write_text("已暂存\n", encoding="utf-8")
        self.git(self.cache, "add", ".")
        with self.assertRaisesRegex(PREPARE.UpstreamError, "修改|干净"):
            self.prepare()

    def test_rejects_changes_hidden_by_index_flags(self) -> None:
        self.prepare()
        self.git(self.cache, "update-index", "--assume-unchanged", "main/key_guard.h")
        (self.cache / "main/key_guard.h").write_text("被标记隐藏的修改\n", encoding="utf-8")
        self.assertEqual(self.git(self.cache, "status", "--porcelain"), "")
        with self.assertRaisesRegex(PREPARE.UpstreamError, "修改|索引"):
            self.prepare()

    def test_rejects_extra_untracked_source(self) -> None:
        self.prepare()
        (self.cache / "unexpected.c").write_text("额外源码\n", encoding="utf-8")
        with self.assertRaisesRegex(PREPARE.UpstreamError, "修改|干净"):
            self.prepare()
        self.assertTrue((self.cache / "unexpected.c").exists())

    def test_rejects_wrong_head(self) -> None:
        self.prepare()
        self.git(self.cache, "config", "user.name", "依赖测试")
        self.git(self.cache, "config", "user.email", "upstream-test@example.invalid")
        self.git(self.cache, "commit", "--quiet", "--allow-empty", "-m", "错误提交")
        with self.assertRaisesRegex(PREPARE.UpstreamError, "提交|HEAD"):
            self.prepare()

    def test_rejects_attached_branch(self) -> None:
        self.prepare()
        self.git(self.cache, "checkout", "--quiet", "-b", "local-change")
        with self.assertRaisesRegex(PREPARE.UpstreamError, "分离|detached"):
            self.prepare()

    def test_rejects_changed_origin(self) -> None:
        self.prepare()
        self.git(self.cache, "remote", "set-url", "origin", "https://example.invalid/other.git")
        with self.assertRaisesRegex(PREPARE.UpstreamError, "来源|origin"):
            self.prepare()

    def test_rejects_existing_nonrepository_directory(self) -> None:
        self.cache.mkdir(parents=True)
        marker = self.cache / "personal.txt"
        marker.write_text("保留\n", encoding="utf-8")
        with self.assertRaises(PREPARE.UpstreamError):
            self.prepare()
        self.assertEqual(marker.read_text(), "保留\n")

    def test_rejects_symlink_cache(self) -> None:
        self.cache.parent.mkdir(parents=True)
        self.cache.symlink_to(self.source, target_is_directory=True)
        with self.assertRaisesRegex(PREPARE.UpstreamError, "链接|目录"):
            self.prepare()
        self.assertTrue(self.cache.is_symlink())

    def test_rejects_nonfixed_or_malformed_commit(self) -> None:
        for commit in ("main", "006c227", "../" + "a" * 40, "g" * 40, None):
            with self.subTest(commit=commit):
                self.write_metadata(baseline_commit=commit)
                with self.assertRaisesRegex(PREPARE.UpstreamError, "40|提交"):
                    self.prepare()
        self.assertFalse(self.cache.parent.exists())

    def test_rejects_invalid_or_credentialed_repository_url(self) -> None:
        for repository in (
            None,
            "",
            "--upload-pack=evil",
            "file:///tmp/source",
            "https://user:secret@example.invalid/repo.git",
            "https://[invalid/repo.git",
            "https://example.invalid/repo.git\n",
        ):
            with self.subTest(repository=repository):
                self.write_metadata(repository=repository)
                with self.assertRaisesRegex(PREPARE.UpstreamError, "repository|URL|来源"):
                    self.prepare()
        self.assertFalse(self.cache.parent.exists())

    def test_rejects_invalid_metadata(self) -> None:
        for text in ("{", "[]", "{}", '{"upstream": []}'):
            with self.subTest(text=text):
                (self.project / "upstream.json").write_text(text, encoding="utf-8")
                with self.assertRaisesRegex(PREPARE.UpstreamError, "upstream.json|upstream"):
                    self.prepare()

    def test_rejects_missing_metadata(self) -> None:
        (self.project / "upstream.json").unlink()
        with self.assertRaisesRegex(PREPARE.UpstreamError, "upstream.json"):
            self.prepare()

    def test_failed_fetch_cleans_temporary_directory_and_can_retry(self) -> None:
        self.cache.parent.mkdir(parents=True)
        marker = self.cache.parent / "another-owner"
        marker.mkdir()
        (marker / "data").write_text("保留\n", encoding="utf-8")
        self.write_metadata(baseline_commit="0" * 40)
        with self.assertRaisesRegex(PREPARE.UpstreamError, "Git|git"):
            self.prepare()
        self.assertEqual(list(self.cache.parent.iterdir()), [marker])
        self.write_metadata()
        self.assertEqual(self.prepare(), self.cache)
        self.assertEqual((marker / "data").read_text(), "保留\n")

    def test_rejects_missing_local_source(self) -> None:
        shutil.rmtree(self.source)
        with self.assertRaisesRegex(PREPARE.UpstreamError, "source|本地"):
            self.prepare()
        self.assertFalse(self.cache.exists())

    def test_default_fetch_uses_locked_repository(self) -> None:
        git = PREPARE._git

        def fetch_locally(directory: Path, *arguments: str) -> str:
            if arguments[0] == "fetch":
                self.assertEqual(arguments[-2:], (self.repository, self.commit))
                arguments = (*arguments[:-2], str(self.source), self.commit)
            return git(directory, *arguments)

        with mock.patch.object(PREPARE, "_git", side_effect=fetch_locally):
            self.assertEqual(PREPARE.prepare_upstream(self.project), self.cache)

    def test_git_unavailable_or_timed_out_leaves_no_partial_cache(self) -> None:
        for error in (
            FileNotFoundError("Git 未安装"),
            subprocess.TimeoutExpired(["git", "init"], 120),
        ):
            with self.subTest(error=error):
                with mock.patch.object(PREPARE.subprocess, "run", side_effect=error):
                    with self.assertRaisesRegex(PREPARE.UpstreamError, "Git"):
                        PREPARE.prepare_upstream(self.project)
                self.assertEqual(list(self.cache.parent.iterdir()), [])

    def test_concurrent_initializer_can_publish_cache_first(self) -> None:
        def another_initializer(staging: Path, destination: Path) -> None:
            shutil.copytree(staging, destination)
            raise FileExistsError("另一进程已经完成准备")

        with mock.patch.object(Path, "rename", another_initializer):
            self.assertEqual(self.prepare(), self.cache)
        self.assertEqual(list(self.cache.parent.iterdir()), [self.cache])
        self.assertEqual(self.prepare(), self.cache)

    def test_concurrent_unrelated_directory_is_preserved(self) -> None:
        def another_owner(staging: Path, destination: Path) -> None:
            destination.mkdir()
            (destination / "personal.txt").write_text("保留\n", encoding="utf-8")
            raise FileExistsError("另一个使用者创建了目录")

        with mock.patch.object(Path, "rename", another_owner):
            with self.assertRaises(PREPARE.UpstreamError):
                self.prepare()
        self.assertEqual((self.cache / "personal.txt").read_text(), "保留\n")
        self.assertEqual(list(self.cache.parent.iterdir()), [self.cache])

    def test_rejects_wrong_repository_structure_and_cleans_up(self) -> None:
        (self.source / "main/key_guard.h").unlink()
        self.git(self.source, "add", ".")
        self.git(self.source, "commit", "--quiet", "-m", "缺少必需头文件")
        self.write_metadata(baseline_commit=self.git(self.source, "rev-parse", "HEAD"))
        with self.assertRaisesRegex(PREPARE.UpstreamError, "key_guard.h|结构"):
            self.prepare()
        self.assertEqual(list(self.cache.parent.iterdir()), [])

    def test_cli_prints_only_absolute_path_and_returns_nonzero_on_error(self) -> None:
        tools = self.project / "tools"
        tools.mkdir()
        script = tools / "prepare_upstream.py"
        shutil.copy2(SCRIPT, script)
        result = subprocess.run(
            [sys.executable, str(script), "--source", str(self.source), "--print-path"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, str(self.cache) + "\n")
        (self.cache / "extra.c").write_text("额外源码\n", encoding="utf-8")
        failed = subprocess.run(
            [sys.executable, str(script), "--print-path"], capture_output=True, text=True
        )
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(failed.stdout, "")
        self.assertIn("修改", failed.stderr)

    def test_cli_without_print_path_reports_success_on_stderr(self) -> None:
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(PREPARE, "__file__", str(self.project / "tools/prepare_upstream.py")):
            with mock.patch.object(sys, "argv", ["prepare_upstream.py", "--source", str(self.source)]):
                with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                    self.assertEqual(PREPARE.main(), 0)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn(str(self.cache), stderr.getvalue())

    def test_cli_failure_returns_nonzero_and_empty_stdout(self) -> None:
        (self.project / "upstream.json").unlink()
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(PREPARE, "__file__", str(self.project / "tools/prepare_upstream.py")):
            with mock.patch.object(sys, "argv", ["prepare_upstream.py", "--print-path"]):
                with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                    self.assertEqual(PREPARE.main(), 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("upstream.json", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
