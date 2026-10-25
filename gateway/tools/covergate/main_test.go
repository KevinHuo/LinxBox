package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func TestParseCoverage(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "coverage.out")
	content := `mode: atomic
github.com/KevinHuo/LinxBox/gateway/internal/domain/identity.go:5.7,8.10 3 2
github.com/KevinHuo/LinxBox/gateway/internal/domain/errors.go:10.7,12.10 2 0
github.com/KevinHuo/LinxBox/gateway/internal/domain/other.go:3.7,4.10 1 1
github.com/KevinHuo/LinxBox/gateway/internal/config/config.go:1.7,2.10 2 2
github.com/other/repo/file.go:1.7,2.10 2 2
bad-line
`
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	cov, err := parseCoverage(path)
	if err != nil {
		t.Fatal(err)
	}
	// 全部自有代码：domain 3+2+1=6、config 2，共 8 语句；覆盖 3+0+1+2=6。
	total, covered := cov.stmts("")
	if total != 8 || covered != 6 {
		t.Fatalf("整体统计错误: %d/%d", covered, total)
	}
	// 关键包前缀过滤：domain 6 语句，覆盖 3+0+1=4。
	total, covered = cov.stmts("github.com/KevinHuo/LinxBox/gateway/internal/domain/")
	if total != 6 || covered != 4 {
		t.Fatalf("domain 统计错误: %d/%d", covered, total)
	}
	if got := pct(total, covered); got < 66.6 || got > 66.7 {
		t.Fatalf("百分比错误: %v", got)
	}
}

func TestPkgPrefixAndSplit(t *testing.T) {
	if got := pkgPrefix("internal/config"); got != "github.com/KevinHuo/LinxBox/gateway/internal/config/" {
		t.Fatalf("前缀错误: %s", got)
	}
	got := splitNonEmpty(" a , b, ,c ")
	if len(got) != 3 || got[0] != "a" || got[1] != "b" || got[2] != "c" {
		t.Fatalf("拆分错误: %v", got)
	}
	if got := splitNonEmpty(""); len(got) != 0 {
		t.Fatalf("空串应返回空: %v", got)
	}
}

func TestParseCoverageUnionDedupe(t *testing.T) {
	// 同一块被多个测试二进制重复记录（-coverpkg 多包合并）：语句数只计一次，
	// 任一运行覆盖即视为已覆盖。
	content := `mode: atomic
github.com/KevinHuo/LinxBox/gateway/internal/domain/identity.go:5.7,8.10 3 2
github.com/KevinHuo/LinxBox/gateway/internal/domain/identity.go:5.7,8.10 3 0
github.com/KevinHuo/LinxBox/gateway/internal/domain/identity.go:5.7,8.10 3 0
github.com/KevinHuo/LinxBox/gateway/internal/domain/errors.go:10.7,12.10 2 0
github.com/KevinHuo/LinxBox/gateway/internal/domain/errors.go:10.7,12.10 2 0
`
	path := filepath.Join(t.TempDir(), "coverage.out")
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	cov, err := parseCoverage(path)
	if err != nil {
		t.Fatal(err)
	}
	// 两个唯一块：identity 3 覆盖、errors 2 未覆盖 → 3/5。
	total, covered := cov.stmts("")
	if total != 5 || covered != 3 {
		t.Fatalf("并集统计错误: %d/%d", covered, total)
	}
}

func TestParseCoverageMissingFile(t *testing.T) {
	if _, err := parseCoverage(filepath.Join(t.TempDir(), "nope.out")); err == nil {
		t.Fatal("缺失文件应报错")
	}
}

func TestPctEdge(t *testing.T) {
	if pct(0, 0) != 100 {
		t.Fatal("无语句时视为 100%")
	}
}

func TestRunGatesSkipZeroStatements(t *testing.T) {
	// 关键包/增量包无语句时跳过，不报错。
	path := writeCov(t, []string{
		"github.com/KevinHuo/LinxBox/gateway/internal/domain/identity.go:1.1,2.2 10 10",
	})
	if err := runGates(path, 90, 90, []string{"internal/does-not-exist"}, ""); err != nil {
		t.Fatalf("零语句关键包应跳过: %v", err)
	}
}

// writeCov 写覆盖率输出文件。
func writeCov(t *testing.T, lines []string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "coverage.out")
	content := "mode: atomic\n"
	for _, l := range lines {
		content += l + "\n"
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

const covDomainAll = "github.com/KevinHuo/LinxBox/gateway/internal/domain/identity.go:1.1,2.2 10 10"

func TestRunGatesPass(t *testing.T) {
	path := writeCov(t, []string{covDomainAll, "github.com/KevinHuo/LinxBox/gateway/internal/config/config.go:1.1,2.2 10 10"})
	if err := runGates(path, 90, 90, []string{"internal/domain", "internal/config"}, ""); err != nil {
		t.Fatalf("应通过: %v", err)
	}
}

func TestRunGatesOverallFail(t *testing.T) {
	// count=0 表示语句未被覆盖。
	path := writeCov(t, []string{
		"github.com/KevinHuo/LinxBox/gateway/internal/domain/identity.go:1.1,2.2 10 0",
	})
	if err := runGates(path, 90, 90, nil, ""); err == nil {
		t.Fatal("整体覆盖率不足应报错")
	}
}

func TestRunGatesKeyPackageFail(t *testing.T) {
	path := writeCov(t, []string{
		"github.com/KevinHuo/LinxBox/gateway/internal/domain/identity.go:1.1,2.2 10 0",
		"github.com/KevinHuo/LinxBox/gateway/internal/config/config.go:1.1,2.2 10 10",
	})
	// 整体 50% 恰好达标（阈值 50 不算低于）；关键包 domain 0% 应触发。
	if err := runGates(path, 50, 90, []string{"internal/domain", "internal/config"}, ""); err == nil {
		t.Fatal("关键包覆盖率不足应报错")
	}
}

func TestRunGatesMissingFile(t *testing.T) {
	if err := runGates(filepath.Join(t.TempDir(), "nope.out"), 90, 90, nil, ""); err == nil {
		t.Fatal("缺失文件应报错")
	}
}

func runGit(t *testing.T, dir string, args ...string) {
	t.Helper()
	cmd := exec.Command("git", args...)
	cmd.Dir = dir
	cmd.Env = append(os.Environ(), "GIT_AUTHOR_DATE=2026-10-25T00:00:00+08:00", "GIT_COMMITTER_DATE=2026-10-25T00:00:00+08:00")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git %v: %v: %s", args, err, out)
	}
}

func TestChangedPackages(t *testing.T) {
	dir := t.TempDir()
	runGit(t, dir, "init", "-q")
	runGit(t, dir, "config", "user.email", "t@example.com")
	runGit(t, dir, "config", "user.name", "t")
	for _, p := range []string{"gateway/internal/config", "gateway/internal/domain", "gateway/tools"} {
		if err := os.MkdirAll(filepath.Join(dir, p), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(dir, "gateway/internal/config/config.go"), []byte("package config\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "gateway/internal/domain/domain.go"), []byte("package domain\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	runGit(t, dir, "add", ".")
	runGit(t, dir, "commit", "-qm", "base")
	// 改动：config 业务文件、config 测试文件、tools 文件。
	if err := os.WriteFile(filepath.Join(dir, "gateway/internal/config/config.go"), []byte("package config\n// change\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "gateway/internal/config/config_test.go"), []byte("package config\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "gateway/tools/x.go"), []byte("package x\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	runGit(t, dir, "add", ".")
	runGit(t, dir, "commit", "-qm", "change")
	t.Chdir(dir)

	got, err := changedPackages("HEAD~1")
	if err != nil {
		t.Fatal(err)
	}
	// 仅 internal/config 应进入增量检查（测试文件与 tools 排除）。
	if len(got) != 1 || got[0] != "internal/config" {
		t.Fatalf("增量包错误: %v", got)
	}
}

func TestRunGatesIncremental(t *testing.T) {
	dir := t.TempDir()
	runGit(t, dir, "init", "-q")
	runGit(t, dir, "config", "user.email", "t@example.com")
	runGit(t, dir, "config", "user.name", "t")
	if err := os.MkdirAll(filepath.Join(dir, "gateway/internal/config"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "gateway/internal/config/config.go"), []byte("package config\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	runGit(t, dir, "add", ".")
	runGit(t, dir, "commit", "-qm", "base")
	if err := os.WriteFile(filepath.Join(dir, "gateway/internal/config/config.go"), []byte("package config\n// change\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	runGit(t, dir, "add", ".")
	runGit(t, dir, "commit", "-qm", "change")
	t.Chdir(dir)
	// 整体 100% 达标、关键包为空，增量包（internal/config）覆盖率不足。
	cov := writeCov(t, []string{
		"github.com/KevinHuo/LinxBox/gateway/internal/domain/identity.go:1.1,2.2 10 10",
		"github.com/KevinHuo/LinxBox/gateway/internal/config/config.go:1.1,2.2 10 0",
	})
	if err := runGates(cov, 10, 10, nil, "HEAD~1"); err == nil {
		t.Fatal("增量包覆盖率不足应报错")
	}
}

func TestParseCoverageMalformed(t *testing.T) {
	// 字段数不足、非数字语句数、非自有代码前缀的行都应跳过。
	path := writeCov(t, []string{
		"github.com/KevinHuo/LinxBox/gateway/internal/domain/identity.go:1.1,2.2 10 10",
		"two-fields-line 1",
		"github.com/KevinHuo/LinxBox/gateway/internal/domain/errors.go:1.1,2.2 x y",
		"github.com/other/repo/file.go:1.1,2.2 5 5",
	})
	cov, err := parseCoverage(path)
	if err != nil {
		t.Fatal(err)
	}
	total, covered := cov.stmts("")
	if total != 10 || covered != 10 {
		t.Fatalf("畸形行应跳过，仅统计合法块: %d/%d", covered, total)
	}
}

func TestChangedPackagesNotGitRepo(t *testing.T) {
	t.Chdir(t.TempDir())
	if _, err := changedPackages("HEAD~1"); err == nil {
		t.Fatal("非 git 目录应报错")
	}
}

func TestRunGatesChangedPackagesError(t *testing.T) {
	// 整体达标但增量基点在非 git 目录：changedPackages 报错。
	dir := t.TempDir()
	path := filepath.Join(dir, "coverage.out")
	content := `mode: atomic
github.com/KevinHuo/LinxBox/gateway/internal/domain/identity.go:5.7,8.10 3 2
github.com/KevinHuo/LinxBox/gateway/internal/domain/errors.go:10.7,12.10 2 0
github.com/KevinHuo/LinxBox/gateway/internal/config/config.go:1.7,2.10 2 0
`
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	cwd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	os.Chdir(dir)
	defer os.Chdir(cwd)
	if err := runGates(path, 10, 10, nil, "HEAD"); err == nil {
		t.Fatal("非 git 目录下增量门禁应报错")
	}
}
