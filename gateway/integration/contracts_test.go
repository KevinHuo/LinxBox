// Package integration 做跨模块验证：共享契约一致性、fixtures 正反例、网络全链路隔离。
package integration

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
)

// repoRoot 返回仓库根目录（gateway 的上一级）。
func repoRoot() string {
	return filepath.Clean(filepath.Join("..", ".."))
}

func TestEmbeddedAssetsMatchSourceContracts(t *testing.T) {
	srcDir := filepath.Join(repoRoot(), "contracts")
	assetsDir := filepath.Join("..", "internal", "contracts", "assets")
	for _, name := range []string{"common.schema.json", "firmware-gateway.schema.json", "desktop-gateway.schema.json", "mcp.schema.json"} {
		src, err := os.ReadFile(filepath.Join(srcDir, name))
		if err != nil {
			t.Fatalf("读取源契约 %s: %v", name, err)
		}
		asset, err := os.ReadFile(filepath.Join(assetsDir, name))
		if err != nil {
			t.Fatalf("读取嵌入资源 %s: %v", name, err)
		}
		if string(src) != string(asset) {
			t.Fatalf("%s 与源契约不一致：请运行 go run ./tools/gencontracts", name)
		}
	}
}

// fixtureCase 是 fixtures 文件中的单条用例。
type fixtureCase struct {
	Name   string          `json:"name"`
	Schema string          `json:"schema"`
	Valid  bool            `json:"valid"`
	Data   json.RawMessage `json:"data"`
}

func loadFixtureCases(t *testing.T, file string) []fixtureCase {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(repoRoot(), "contracts", "fixtures", file))
	if err != nil {
		t.Fatal(err)
	}
	var out []fixtureCase
	if err := json.Unmarshal(data, &out); err != nil {
		t.Fatalf("%s 结构不识别: %v", file, err)
	}
	return out
}

// TestFixturesValidateAgainstEmbeddedSchemas 用嵌入 Schema 复核全部 fixtures 正反例。
func TestFixturesValidateAgainstEmbeddedSchemas(t *testing.T) {
	v, err := contracts.NewValidator()
	if err != nil {
		t.Fatal(err)
	}
	for _, file := range []string{"boundaries.json", "firmware-gateway.json", "desktop-gateway.json", "mcp.json"} {
		for _, c := range loadFixtureCases(t, file) {
			schemaFile, def, ok := strings.Cut(c.Schema, "#/$defs/")
			if !ok {
				t.Fatalf("%s: schema 字段无法解析: %s", file, c.Schema)
			}
			err := v.ValidateFragment(schemaFile, def, c.Data)
			if c.Valid && err != nil {
				t.Errorf("%s「%s」正例被拒: %v", file, c.Name, err)
			}
			if !c.Valid && err == nil {
				t.Errorf("%s「%s」反例应被拒", file, c.Name)
			}
		}
	}
}

func TestUnknownFixtureFile(t *testing.T) {
	v, err := contracts.NewValidator()
	if err != nil {
		t.Fatal(err)
	}
	if err := v.ValidateFragment("no.such.json", "X", []byte("{}")); err == nil {
		t.Fatal("未知 Schema 文件应报错")
	}
}
