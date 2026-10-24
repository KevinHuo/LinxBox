#!/usr/bin/env python3
"""检查本项目维护的构建输入、中文文档、安全内容与固件工作流。"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
REQUIRED_FILES = (
    "README.md", "AGENTS.md", "LICENSE", "CMakeLists.txt", "upstream.json",
    "sdkconfig.defaults", "partitions.csv", "dependencies.lock",
    "main/main.c", "main/CMakeLists.txt",
    "hardware/folotoy/ai-passport/board.cmake", "tools/prepare_upstream.py",
)
LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
CJK_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff]")
SECRET_PATTERNS = (
    re.compile(r"(?:ghp_|github_pat_)[A-Za-z0-9_]{20,}"),
    re.compile(r"AKIA[0-9A-Z]{16}"),
    re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    re.compile(r"https://ai-passport\.folotoy\.cn/trae/\?s=(?!<|AAAAAA)[^&\s)]+&k=[^\s)]+"),
)
CONFLICT_RE = re.compile(r"(?m)^(<<<<<<< |=======\s*$|>>>>>>> )")


def git_files(root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=root, check=True, capture_output=True, text=True,
    )
    return [root / name for name in result.stdout.split("\0") if name]


def check_project(root: Path) -> list[str]:
    errors: list[str] = []
    for name in REQUIRED_FILES:
        if not (root / name).is_file():
            errors.append(f"缺少必要文件：{name}")
    ignored = subprocess.run(
        ["git", "check-ignore", "-q", "dependencies.lock"], cwd=root,
        capture_output=True,
    )
    if ignored.returncode == 0:
        errors.append("dependencies.lock 必须纳入 Git，不能忽略")
    elif ignored.returncode != 1:
        raise subprocess.CalledProcessError(ignored.returncode, "git check-ignore")
    git_root = Path(subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], cwd=root,
        check=True, capture_output=True, text=True,
    ).stdout.strip())

    for path in git_files(root):
        if not path.is_file() or path.stat().st_size > 2 * 1024 * 1024:
            continue
        try:
            content = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        name = path.relative_to(root).as_posix()
        if any(pattern.search(content) for pattern in SECRET_PATTERNS):
            errors.append(f"{name}：可能含敏感凭证或设备二维码秘密")
        if CONFLICT_RE.search(content):
            errors.append(f"{name}：存在未解决的合并冲突")
        if path.suffix.lower() != ".md":
            continue
        prose = re.sub(r"\A---\n.*?\n---(?:\n|$)", "", content, flags=re.DOTALL)
        prose = re.sub(
            r"(?ms)^ {0,3}(?:`{3,}|~{3,})[^\n]*\n.*?^ {0,3}(?:`{3,}|~{3,})[ \t]*$",
            "", prose,
        )
        if not CJK_RE.search(prose):
            errors.append(f"{name}：文档必须使用中文正文")
        for raw in LINK_RE.findall(content):
            raw = raw.strip()
            if raw.startswith("<") and ">" in raw:
                target = raw[1:raw.index(">")]
            else:
                target = raw.split(maxsplit=1)[0] if raw else ""
            if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            local = unquote(target.split("#", 1)[0])
            resolved = git_root / local.lstrip("/") if local.startswith("/") else path.parent / local
            if local and not resolved.exists():
                errors.append(f"{name}：本地链接不存在")

    workflow = git_root / ".github/workflows/firmware.yml"
    if workflow.is_file():
        for number, line in enumerate(workflow.read_text(encoding="utf-8").splitlines(), 1):
            match = re.match(r"\s*-?\s*uses:\s*['\"]?([^'\"\s]+)", line)
            if not match or match.group(1).startswith("./"):
                continue
            action = match.group(1)
            pattern = r"@sha256:[0-9a-f]{64}$" if action.startswith("docker://") else r"@[0-9a-f]{40}$"
            pinned = re.search(pattern, action)
            if not pinned:
                errors.append(f".github/workflows/firmware.yml:{number}：操作必须使用固定提交或镜像摘要")
    return errors


def main(root: Path = ROOT) -> int:
    try:
        errors = check_project(root)
    except (OSError, subprocess.CalledProcessError):
        print("错误：无法读取项目文件或运行 Git 检查", file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(f"错误：{error}", file=sys.stderr)
        return 1
    print("本项目检查：通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
