#!/usr/bin/env python3
"""准备并校验 upstream.json 锁定的 AI Passport 上游依赖。"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import urlsplit


REQUIRED_FILES = (
    "CMakeLists.txt",
    "LICENSE",
    "components/bsp/CMakeLists.txt",
    "main/key_guard.h",
    "tools/validate.sh",
    "tools/verify_firmware.py",
    "tools/archive_firmware.py",
)


class UpstreamError(RuntimeError):
    """依赖配置、获取或缓存校验失败。"""


def _read_metadata(project_root: Path) -> tuple[str, str]:
    try:
        metadata = json.loads((project_root / "upstream.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise UpstreamError(f"无法读取 upstream.json：{error}") from error
    if not isinstance(metadata, dict) or not isinstance(metadata.get("upstream"), dict):
        raise UpstreamError("upstream.json 必须包含 upstream 对象")
    upstream = metadata["upstream"]
    commit = upstream.get("baseline_commit")
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", commit):
        raise UpstreamError("upstream.baseline_commit 必须为固定的 40 位 Git 提交 SHA")
    repository = upstream.get("repository")
    if not isinstance(repository, str) or any(character.isspace() for character in repository):
        raise UpstreamError("upstream.repository 必须为不带凭据的 HTTPS 来源 URL")
    try:
        url = urlsplit(repository)
        valid = (
            url.scheme == "https"
            and url.hostname
            and url.path not in ("", "/")
            and not url.username
            and not url.password
            and not url.query
            and not url.fragment
        )
    except ValueError:
        valid = False
    if not valid:
        raise UpstreamError("upstream.repository 必须为不带凭据的 HTTPS 来源 URL")
    return repository, commit.lower()


def _git(directory: Path, *arguments: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(directory), *arguments],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=120,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise UpstreamError(f"无法执行 Git {arguments[0]}：{error}") from error
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise UpstreamError(f"Git {arguments[0]} 失败：{detail}")
    return result.stdout.strip()


def _validate_checkout(directory: Path, repository: str, commit: str) -> None:
    if (
        directory.is_symlink()
        or not directory.is_dir()
        or (directory / ".git").is_symlink()
        or not (directory / ".git").is_dir()
    ):
        raise UpstreamError(f"上游缓存必须为独立 Git 目录，不能是链接：{directory}")
    if Path(_git(directory, "rev-parse", "--show-toplevel")).resolve() != directory.resolve():
        raise UpstreamError(f"上游缓存不是独立仓库根目录：{directory}")
    if _git(directory, "rev-parse", "--verify", "HEAD") != commit:
        raise UpstreamError(f"上游缓存 HEAD 与锁定提交不一致：{directory}")
    if _git(directory, "rev-parse", "--abbrev-ref", "HEAD") != "HEAD":
        raise UpstreamError(f"上游缓存必须使用分离 HEAD（detached）：{directory}")
    if _git(directory, "config", "--get", "remote.origin.url") != repository:
        raise UpstreamError(f"上游缓存 origin 来源与 upstream.json 不一致：{directory}")
    if _git(directory, "status", "--porcelain=v1", "--untracked-files=all"):
        raise UpstreamError(f"上游缓存存在修改或额外文件，请保留改动后另行处理：{directory}")
    if any(line and line[0] != "H" for line in _git(directory, "ls-files", "-v").splitlines()):
        raise UpstreamError(f"上游缓存含跳过文件校验的索引标记，无法确认无修改：{directory}")
    for name in REQUIRED_FILES:
        if not (directory / name).is_file():
            raise UpstreamError(f"上游仓库结构不完整，缺少 {name}：{directory}")


def prepare_upstream(project_root: Path, source: str | None = None) -> Path:
    """为固件目录准备固定依赖；已验证缓存可离线复用，不修改锁定版本。"""
    project_root = project_root.resolve()
    repository, commit = _read_metadata(project_root)
    cache_parent = project_root.parent / ".local" / "dependencies" / "ai-passport"
    destination = cache_parent / commit
    if destination.exists() or destination.is_symlink():
        _validate_checkout(destination, repository, commit)
        return destination

    fetch_source = repository
    if source is not None:
        local_source = Path(source).expanduser().resolve()
        if not local_source.is_dir():
            raise UpstreamError(f"--source 本地仓库目录不存在：{local_source}")
        _git(local_source, "rev-parse", "--git-dir")
        fetch_source = str(local_source)

    print(f"准备固定上游依赖 {commit}……", file=sys.stderr)
    cache_parent.mkdir(parents=True, exist_ok=True)
    # 只清理由本次调用创建的临时目录，失败不留下半成品缓存。
    with tempfile.TemporaryDirectory(prefix=f".{commit}-", dir=cache_parent) as temporary:
        staging = Path(temporary)
        _git(staging, "init", "--quiet")
        _git(staging, "remote", "add", "origin", repository)
        _git(staging, "fetch", "--depth=1", "--no-tags", "--", fetch_source, commit)
        _git(staging, "checkout", "--quiet", "--detach", commit)
        _validate_checkout(staging, repository, commit)
        if destination.exists() or destination.is_symlink():
            _validate_checkout(destination, repository, commit)
        else:
            try:
                staging.rename(destination)
            except OSError:
                # 并发准备成功时复用对方的完整缓存，不覆盖任何已有文件。
                if not destination.exists() and not destination.is_symlink():
                    raise
                _validate_checkout(destination, repository, commit)
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    options = parser.add_argument_group("选项")
    options.add_argument("-h", "--help", action="help", help="显示帮助并退出")
    options.add_argument("--print-path", action="store_true", help="只向标准输出打印已验证的依赖绝对路径")
    options.add_argument("--source", metavar="本地仓库", help="从本地 Git 仓库获取相同固定提交，用于离线初始化")
    arguments = parser.parse_args()
    try:
        path = prepare_upstream(Path(__file__).resolve().parents[1], arguments.source)
    except (UpstreamError, OSError) as error:
        print(f"上游依赖准备失败：{error}", file=sys.stderr)
        return 1
    if arguments.print_path:
        print(path)
    else:
        print(f"上游依赖已就绪：{path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
