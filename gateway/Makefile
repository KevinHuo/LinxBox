# linxbox-gateway 工程命令。
# make generate/check/build/release 均依赖生成步骤。

GO ?= go
BIN := bin/linxbox-gateway

.PHONY: help generate check build release smoke smoke-local clean test race vet fmt

help:
	@echo "generate  复制共享 Schema 到 internal/contracts/assets"
	@echo "check     格式、静态检查、单元/集成测试、覆盖率与 race 门禁"
	@echo "build     生成本机 bin/linxbox-gateway"
	@echo "release   无 CGO 交叉构建 Linux amd64/arm64 到 dist/"
	@echo "smoke     构建后通过真实网络入口运行模拟链路（需 config.local.json）"
	@echo "smoke-local  使用临时数据库和模拟两端，验证本机 TCP 链路、重复运行与重启恢复"
	@echo "test      运行全部测试"
	@echo "race      运行带数据竞争检测的测试"
	@echo "fmt       格式化源码"

generate:
	$(GO) run ./tools/gencontracts

check: generate
	./scripts/check.sh

build: generate
	mkdir -p bin
	$(GO) build -trimpath -o $(BIN) ./cmd/linxbox-gateway

release: generate
	mkdir -p dist
	CGO_ENABLED=0 GOOS=linux GOARCH=amd64 $(GO) build -trimpath -o dist/linux-amd64/linxbox-gateway ./cmd/linxbox-gateway
	CGO_ENABLED=0 GOOS=linux GOARCH=arm64 $(GO) build -trimpath -o dist/linux-arm64/linxbox-gateway ./cmd/linxbox-gateway
	@echo "release: dist/linux-amd64/linxbox-gateway, dist/linux-arm64/linxbox-gateway"

smoke: build
	@if [ ! -f config.local.json ]; then echo "缺少 config.local.json（参考 config.example.json）"; exit 1; fi
	$(GO) run ./cmd/linxbox-smoke --config ./config.local.json

smoke-local: generate
	$(GO) test -count=1 -v ./cmd/linxbox-smoke -run '^TestSmokerAgainstLiveServer$$'

test: generate
	$(GO) test ./...

race: generate
	$(GO) test -race ./...

vet: generate
	$(GO) vet ./...

fmt:
	gofmt -w ./cmd ./internal ./integration ./tools

clean:
	rm -rf bin dist coverage.out
