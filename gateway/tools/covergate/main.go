// Command covergate 解析 go test 覆盖率输出，执行整体、关键包与增量覆盖率门禁。
// 增量检查：--base <ref> 时，对相对该 ref 变更的自有 Go 包（不含测试辅助与工具除外项）
// 分别要求达到 --package-min。
package main

import (
	"bufio"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path"
	"sort"
	"strconv"
	"strings"
)

func main() {
	coverFile := flag.String("cover", "coverage.out", "go test 覆盖率输出文件")
	overallMin := flag.Float64("overall-min", 90, "整体覆盖率下限（%）")
	packageMin := flag.Float64("package-min", 90, "关键包覆盖率下限（%）")
	keyPackages := flag.String("key-packages", "", "关键包列表（逗号分隔，内部路径）")
	base := flag.String("base", "", "PR 基线 ref；设置时启用增量检查")
	flag.Parse()

	if err := runGates(*coverFile, *overallMin, *packageMin, splitNonEmpty(*keyPackages), *base); err != nil {
		fail(err)
	}
}

// runGates 执行整体、关键包与增量门禁；拆出便于测试。
func runGates(coverFile string, overallMin, packageMin float64, keyPackages []string, base string) error {
	cov, err := parseCoverage(coverFile)
	if err != nil {
		return err
	}

	// 整体门禁：统计范围内全部自有可测代码。
	total, covered := cov.stmts("")
	if pct(total, covered) < overallMin {
		return fmt.Errorf("整体覆盖率 %.2f%% 低于 %.0f%%", pct(total, covered), overallMin)
	}
	fmt.Printf("整体覆盖率: %.2f%% (%d/%d)\n", pct(total, covered), covered, total)

	// 关键包门禁。
	for _, k := range keyPackages {
		t, c := cov.stmts(pkgPrefix(k))
		if t == 0 {
			continue
		}
		if pct(t, c) < packageMin {
			return fmt.Errorf("关键包 %s 覆盖率 %.2f%% 低于 %.0f%%", k, pct(t, c), packageMin)
		}
		fmt.Printf("关键包 %s: %.2f%% (%d/%d)\n", k, pct(t, c), c, t)
	}

	// 增量门禁。
	if base != "" {
		changed, err := changedPackages(base)
		if err != nil {
			return err
		}
		for _, p := range changed {
			t, c := cov.stmts(pkgPrefix(p))
			if t == 0 {
				continue
			}
			if pct(t, c) < packageMin {
				return fmt.Errorf("增量包 %s 覆盖率 %.2f%% 低于 %.0f%%", p, pct(t, c), packageMin)
			}
			fmt.Printf("增量包 %s: %.2f%% (%d/%d)\n", p, pct(t, c), c, t)
		}
	}
	return nil
}

// pkgPrefix 将内部路径前缀转为文件路径前缀（用于匹配 coverage.out 行）。
func pkgPrefix(pkg string) string {
	return "github.com/KevinHuo/LinxBox/gateway/" + pkg + "/"
}

type coverage struct {
	// 每个唯一块（file:positions）一条记录：语句数与覆盖标记（count>0 视为已覆盖）。
	// 多包 -coverpkg 会为同一块生成多条记录，统计时去重后取并集。
	stmtsByFile map[string]stmtStat
	seen        map[string]bool
	coveredSeen map[string]bool
}

type stmtStat struct {
	total   int
	covered int
}

func parseCoverage(file string) (*coverage, error) {
	f, err := os.Open(file)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	c := &coverage{
		stmtsByFile: map[string]stmtStat{},
		seen:        map[string]bool{},
		coveredSeen: map[string]bool{},
	}
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := sc.Text()
		if !strings.HasPrefix(line, "github.com/KevinHuo/LinxBox/gateway/") {
			continue
		}
		// 格式: <pkg>/<file>:<sline>.<scol>,<eline>.<ecol> <num> <count>
		fields := strings.Fields(line)
		if len(fields) != 3 {
			continue
		}
		num, err1 := strconv.Atoi(fields[1])
		count, err2 := strconv.Atoi(fields[2])
		if err1 != nil || err2 != nil {
			continue
		}
		// 块去重：语句数只计一次。
		st := c.stmtsByFile[fields[0]]
		if !c.seen[fields[0]] {
			c.seen[fields[0]] = true
			st.total += num
		}
		// 任一运行覆盖该块即计入覆盖。
		if count > 0 && !c.coveredSeen[fields[0]] {
			c.coveredSeen[fields[0]] = true
			st.covered += num
		}
		c.stmtsByFile[fields[0]] = st
	}
	return c, sc.Err()
}

// stmts 返回匹配 prefix 前缀的文件统计；prefix 为空表示全部。
func (c *coverage) stmts(prefix string) (total, covered int) {
	for file, st := range c.stmtsByFile {
		if prefix == "" || strings.HasPrefix(file, prefix) {
			total += st.total
			covered += st.covered
		}
	}
	return total, covered
}

// changedPackages 返回相对 base 变更的自有 Go 包（排除测试辅助与工具）。
func changedPackages(base string) ([]string, error) {
	cmd := exec.Command("git", "merge-base", "HEAD", base)
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("计算基线 %s: %w", base, err)
	}
	mergeBase := strings.TrimSpace(string(out))
	// 门禁由 gateway/ 执行；路径限定到仓库根目录，避免误匹配 gateway/gateway 而漏检。
	cmd = exec.Command("git", "diff", "--name-only", mergeBase+"..HEAD", "--", ":(top)gateway")
	out, err = cmd.Output()
	if err != nil {
		return nil, err
	}
	set := map[string]struct{}{}
	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasSuffix(line, ".go") || strings.HasSuffix(line, "_test.go") {
			continue
		}
		if !strings.HasPrefix(line, "gateway/") {
			continue
		}
		rel := strings.TrimPrefix(line, "gateway/")
		if strings.HasPrefix(rel, "internal/testutil/") || strings.HasPrefix(rel, "tools/") {
			continue
		}
		dir := path.Dir(rel)
		if dir == "." || dir == "cmd" {
			continue
		}
		set[dir] = struct{}{}
	}
	outList := make([]string, 0, len(set))
	for p := range set {
		outList = append(outList, p)
	}
	sort.Strings(outList)
	return outList, nil
}

func splitNonEmpty(s string) []string {
	var out []string
	for _, p := range strings.Split(s, ",") {
		if t := strings.TrimSpace(p); t != "" {
			out = append(out, t)
		}
	}
	return out
}

func pct(total, covered int) float64 {
	if total == 0 {
		return 100
	}
	return float64(covered) / float64(total) * 100
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "覆盖率门禁未通过:", err)
	os.Exit(1)
}
