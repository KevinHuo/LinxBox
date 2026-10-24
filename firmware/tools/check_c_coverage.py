#!/usr/bin/env python3
"""校验 main/ 与 hardware/ 全部自有 C 源码的行、分支和函数覆盖率。"""

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
MINIMUM = 90
METRICS = ("lines", "branches", "functions")


def evaluate(payload, source):
    expected = {
        path.resolve(): path.relative_to(source).as_posix()
        for directory in (source / "main", source / "hardware")
        for path in directory.rglob("*.c")
    }
    if not expected:
        raise ValueError("没有发现自有 C 应用源码")
    if (not isinstance(payload, dict) or payload.get("type") != "llvm.coverage.json.export"
            or not str(payload.get("version", "")).startswith("2.")
            or not isinstance(payload.get("data"), list) or len(payload["data"]) != 1
            or not isinstance(payload["data"][0], dict)
            or not isinstance(payload["data"][0].get("files"), list)):
        raise ValueError("不是支持的 LLVM 覆盖率 JSON 报告")
    files = {}
    errors = []
    totals = {name: {"count": 0, "covered": 0} for name in METRICS}
    for entry in payload["data"][0]["files"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("filename"), str):
            raise ValueError("覆盖率报告缺少源码路径")
        path = Path(entry["filename"]).resolve()
        if path not in expected:
            continue
        name = expected[path]
        if name in files:
            raise ValueError("覆盖率报告出现重复源码：" + name)
        summary = entry.get("summary")
        if not isinstance(summary, dict):
            raise ValueError("源码缺少覆盖率统计：" + name)
        files[name] = {}
        for metric in METRICS:
            values = summary.get(metric)
            if not isinstance(values, dict):
                raise ValueError(f"{name} 缺少 {metric} 统计")
            count, covered = values.get("count"), values.get("covered")
            if (type(count) is not int or type(covered) is not int
                    or not 0 <= covered <= count or (metric != "branches" and count == 0)):
                raise ValueError(f"{name} 的 {metric} 计数无效")
            percent = covered * 100 / count if count else 100.0
            files[name][metric] = {"count": count, "covered": covered, "percent": percent}
            totals[metric]["count"] += count
            totals[metric]["covered"] += covered
            if covered * 100 < count * MINIMUM:
                errors.append(f"{name} {metric}：{percent:.2f}% 未达到 {MINIMUM}%")
    for name in sorted(set(expected.values()) - set(files)):
        errors.append("源码未进入覆盖率报告：" + name)
    for metric, values in totals.items():
        count, covered = values["count"], values["covered"]
        values["percent"] = covered * 100 / count if count else 100.0
        if covered * 100 < count * MINIMUM:
            errors.append(f"整体 {metric}：{values['percent']:.2f}% 未达到 {MINIMUM}%")
    return {"threshold": MINIMUM, "files": files, "totals": totals, "errors": errors}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="llvm-cov export 的完整 JSON 报告")
    parser.add_argument("--summary", type=Path, help="保存固定口径的覆盖率摘要")
    arguments = parser.parse_args(argv)
    try:
        result = evaluate(json.loads(arguments.report.read_text()), ROOT)
        if arguments.summary:
            arguments.summary.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    except (OSError, ValueError) as error:
        print("覆盖率检查失败：" + str(error), file=sys.stderr)
        return 1
    for name, values in sorted(result["files"].items()):
        print(f"{name}：行 {values['lines']['percent']:.2f}% / "
              f"分支 {values['branches']['percent']:.2f}% / 函数 {values['functions']['percent']:.2f}%")
    for error in result["errors"]:
        print("错误：" + error)
    print("C 应用覆盖率：" + ("失败" if result["errors"] else "通过"))
    return 1 if result["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
