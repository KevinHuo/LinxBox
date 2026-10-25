// Command gencontracts 将根目录 contracts/ 的共享 JSON Schema 复制到
// gateway/internal/contracts/assets/ 供 go:embed 使用，并校验复制结果与源文件字节一致。
//
// 生成资源不入 Git，不得手改；契约正文始终以根目录 contracts/ 为准。
package main

import (
	"bytes"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
)

func main() {
	_, thisFile, _, ok := runtime.Caller(0)
	if !ok {
		fmt.Fprintln(os.Stderr, "无法定位本工具源码路径")
		os.Exit(1)
	}
	// gateway/tools/gencontracts -> gateway/
	gatewayRoot := filepath.Dir(filepath.Dir(filepath.Dir(thisFile)))

	contractsDir := flag.String("contracts", filepath.Join(gatewayRoot, "..", "contracts"), "共享契约源目录")
	assetsDir := flag.String("assets", filepath.Join(gatewayRoot, "internal", "contracts", "assets"), "生成资源目录")
	flag.Parse()

	if err := run(*contractsDir, *assetsDir); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

// run 复制全部 *.schema.json 并核对字节一致。
func run(contractsDir, assetsDir string) error {
	matches, err := filepath.Glob(filepath.Join(contractsDir, "*.schema.json"))
	if err != nil {
		return err
	}
	if len(matches) == 0 {
		return fmt.Errorf("契约源目录 %s 未找到 *.schema.json", contractsDir)
	}
	if err := os.MkdirAll(assetsDir, 0o755); err != nil {
		return err
	}
	for _, src := range matches {
		data, err := os.ReadFile(src)
		if err != nil {
			return err
		}
		name := filepath.Base(src)
		dst := filepath.Join(assetsDir, name)
		if err := os.WriteFile(dst, data, 0o644); err != nil {
			return err
		}
		copied, err := os.ReadFile(dst)
		if err != nil {
			return err
		}
		if !bytes.Equal(data, copied) {
			return fmt.Errorf("%s 复制后字节不一致", name)
		}
		fmt.Printf("generate: %s\n", name)
	}
	return nil
}
