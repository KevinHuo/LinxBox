#!/usr/bin/env bash
# Gateway 检查门禁：格式、静态检查、生成核对、单元/集成测试、覆盖率与 race。
# 覆盖率统计口径：整体与关键包均不低于 90%；设置 INCREMENTAL_BASE 时追加增量门禁。
set -euo pipefail
cd "$(dirname "$0")/.."

KEY_PACKAGES="cmd/linxbox-gateway,cmd/linxbox-smoke,internal/config,internal/contracts,internal/domain,internal/lifecycle,internal/service,internal/storage,internal/transport"

echo "==> 生成资源核对"
go run ./tools/gencontracts

echo "==> gofmt"
files=$(gofmt -l ./cmd ./internal ./integration ./tools 2>/dev/null || true)
if [ -n "$files" ]; then
  echo "以下文件未通过 gofmt："
  echo "$files"
  exit 1
fi

echo "==> go vet"
go vet ./...

echo "==> 单元与集成测试（覆盖率）"
# -count=1 禁用测试缓存：缓存命中的包不会重写覆盖数据，会导致 coverage.out 缺失大量包。
go test -count=1 -covermode=atomic -coverpkg=./... -coverprofile=coverage.out ./...

echo "==> 覆盖率门禁"
if [ -n "${INCREMENTAL_BASE:-}" ]; then
  go run ./tools/covergate \
    -cover coverage.out \
    -overall-min 90 \
    -package-min 90 \
    -key-packages "$KEY_PACKAGES" \
    -base "$INCREMENTAL_BASE"
else
  go run ./tools/covergate \
    -cover coverage.out \
    -overall-min 90 \
    -package-min 90 \
    -key-packages "$KEY_PACKAGES"
fi

echo "==> race 检测（需 C 编译器与 CGO）"
if [ "${CGO_ENABLED:-1}" != "0" ] && command -v cc >/dev/null 2>&1; then
  go test -race ./...
else
  echo "跳过 race：CGO 已禁用或缺少 C 编译器；发布构建与 race 检测本就分开。"
fi

echo "check 通过"
