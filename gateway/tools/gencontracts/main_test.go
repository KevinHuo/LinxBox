package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

func TestRunCopiesSchemas(t *testing.T) {
	src := t.TempDir()
	// 构造两个契约文件。
	files := map[string]string{
		"common.schema.json":           `{"$schema":"draft-2020-12","$id":"https://x/common.schema.json"}`,
		"firmware-gateway.schema.json": `{"$schema":"draft-2020-12","$id":"https://x/fw.schema.json"}`,
		"not-a-schema.txt":             "ignore me",
	}
	for name, content := range files {
		if err := os.WriteFile(filepath.Join(src, name), []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	dst := filepath.Join(t.TempDir(), "assets")
	if err := run(src, dst); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"common.schema.json", "firmware-gateway.schema.json"} {
		got, err := os.ReadFile(filepath.Join(dst, name))
		if err != nil {
			t.Fatalf("%s 未生成: %v", name, err)
		}
		if !bytes.Equal(got, []byte(files[name])) {
			t.Fatalf("%s 字节不一致", name)
		}
	}
	// 非 schema 文件不复制。
	if _, err := os.Stat(filepath.Join(dst, "not-a-schema.txt")); !os.IsNotExist(err) {
		t.Fatal("非 schema 文件不应复制")
	}
}

func TestRunMissingSource(t *testing.T) {
	if err := run(filepath.Join(t.TempDir(), "empty"), t.TempDir()); err == nil {
		t.Fatal("无契约源应报错")
	}
}

func TestRunMkdirError(t *testing.T) {
	// assets 路径的父级是文件：MkdirAll 失败。
	src := t.TempDir()
	if err := os.WriteFile(filepath.Join(src, "a.schema.json"), []byte("{}"), 0o644); err != nil {
		t.Fatal(err)
	}
	blocker := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(blocker, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := run(src, filepath.Join(blocker, "assets")); err == nil {
		t.Fatal("MkdirAll 失败应报错")
	}
}

func TestRunReadError(t *testing.T) {
	// Glob 命中一个目录名：ReadFile 失败。
	src := t.TempDir()
	if err := os.MkdirAll(filepath.Join(src, "a.schema.json"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := run(src, filepath.Join(t.TempDir(), "assets")); err == nil {
		t.Fatal("读取失败应报错")
	}
}

func TestRunWriteError(t *testing.T) {
	src := t.TempDir()
	if err := os.WriteFile(filepath.Join(src, "a.schema.json"), []byte("{}"), 0o644); err != nil {
		t.Fatal(err)
	}
	dst := t.TempDir()
	if err := os.Chmod(dst, 0o500); err != nil {
		t.Fatal(err)
	}
	defer os.Chmod(dst, 0o755)
	if err := run(src, dst); err == nil {
		t.Fatal("写入失败应报错")
	}
}

func TestRunGlobError(t *testing.T) {
	src := filepath.Join(t.TempDir(), "[bad")
	if err := os.MkdirAll(src, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := run(src, t.TempDir()); err == nil {
		t.Fatal("非法 Glob 模式应报错")
	}
}
