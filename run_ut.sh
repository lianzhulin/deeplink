#!/usr/bin/env bash
#
# run_ut.sh — 编译运行 ModuleController 驱动单测并生成代码覆盖率报告
#
# 功能：
#   1. 使用 gcc --coverage 编译驱动 + 测试
#   2. 运行单测二进制
#   3. 收集 gcov 数据 → coverage.info（开启 branch_coverage=1）
#   4. 生成 coverage_html/ 报告（语句覆盖率 + 分支覆盖率分开显示）
#   5. 在终端打印 module_driver.c 的独立汇总（目标文件覆盖率指标）
#
# 使用：
#   bash run_ut.sh
#
# 依赖：gcc, gcov, lcov, genhtml（apt-get install lcov）
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

COV_EXE="test_module_driver_cov"
INFO_FILE="coverage.info"
HTML_DIR="coverage_html"

# -------- 颜色输出（仅当 stdout 为终端时启用）--------
if [ -t 1 ]; then
    RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; BOLD=$'\033[1m'; RESET=$'\033[0m'
else
    RED=""; GREEN=""; YELLOW=""; BOLD=""; RESET=""
fi
info()  { echo "${GREEN}[INFO ]${RESET} $*"; }
warn()  { echo "${YELLOW}[WARN ]${RESET} $*"; }
err()   { echo "${RED}[ERROR]${RESET} $*" >&2; }
title() { echo -e "\n${BOLD}========== $* ==========${RESET}"; }

# -------- 工具链检查 --------
for TOOL in gcc gcov lcov genhtml; do
    if ! command -v "$TOOL" >/dev/null 2>&1; then
        err "缺少依赖：$TOOL 。请先安装（apt-get update && apt-get install -y lcov build-essential）"
        exit 1
    fi
done
info "工具链就绪：gcc=$(gcc -dumpversion) lcov=$(lcov --version | head -1 | awk '{print $NF}')"

# -------- 清理旧产物 --------
title "清理旧覆盖率产物"
rm -f *.gcno *.gcda *.gcov "$INFO_FILE" "$COV_EXE" "test_module_driver" "test_module_driver_plain"
rm -rf "$HTML_DIR"
info "已清理"

# -------- 编译（开启 coverage）--------
title "编译：gcc --coverage"
gcc -std=c99 -Wall -Wextra -Werror -I. -O0 --coverage \
    -o "$COV_EXE" \
    test_module_driver.c module_driver.c
info "编译成功：$SCRIPT_DIR/$COV_EXE"

# -------- 运行单测 --------
title "运行单元测试"
if ./"$COV_EXE"; then
    info "单元测试全部通过"
else
    err "单元测试失败，请根据上方输出修复用例"
    exit 2
fi

# -------- 收集覆盖率数据（含分支）--------
title "收集覆盖率数据（lcov，含 branch_coverage）"
lcov --capture \
     --directory . \
     --output-file "$INFO_FILE" \
     --rc branch_coverage=1 \
     --quiet
info "已生成 $INFO_FILE"

# -------- 抽取 module_driver.c 的独立覆盖率（排除测试文件自身）--------
DRIVER_INFO="/tmp/module_driver_coverage.info"
# 使用 lcov --remove 移除 test_module_driver.c（更可靠，兼容 lcov 1.x / 2.x）
lcov --remove "$INFO_FILE" "*test_module_driver.c" \
     --output-file "$DRIVER_INFO" \
     --rc branch_coverage=1 \
     --quiet

# -------- 生成 HTML 报告（语句 + 分支分开显示）--------
title "生成 HTML 覆盖率报告（语句和分支分开显示）"
genhtml "$INFO_FILE" \
    --branch-coverage \
    --output-directory "$HTML_DIR" \
    --show-details \
    --title "ModuleController Driver Coverage Report" \
    --legend \
    --frames \
    --quiet

# 同时再为 module_driver.c 单独生成一份子报告（方便快速跳转）
genhtml "$DRIVER_INFO" \
    --branch-coverage \
    --output-directory "$HTML_DIR/driver_only" \
    --show-details \
    --title "module_driver.c Coverage (Lines & Branches Separated)" \
    --legend \
    --frames \
    --quiet

info "总报告：  $SCRIPT_DIR/$HTML_DIR/index.html"
info "驱动单独：$SCRIPT_DIR/$HTML_DIR/driver_only/index.html"

# -------- 终端打印汇总（分开 lines / functions / branches）--------
title "覆盖率汇总（分开显示 Lines、Functions、Branches）"

echo ""
echo "────────────────────────────────────────────────────────────"
echo "  整体 (module_driver.c + test_module_driver.c)"
echo "────────────────────────────────────────────────────────────"
lcov --summary "$INFO_FILE" --rc branch_coverage=1 2>&1 | \
    sed -E 's/lines\.*: */语句覆盖率 (Line Coverage)  : /;
            s/functions\.*: */函数覆盖率 (Func Coverage)  : /;
            s/branches\.*: */分支覆盖率 (Branch Coverage): /'

echo ""
echo "────────────────────────────────────────────────────────────"
echo "  目标文件 module_driver.c（不含测试文件自身）"
echo "────────────────────────────────────────────────────────────"
lcov --summary "$DRIVER_INFO" --rc branch_coverage=1 2>&1 | \
    sed -E 's/lines\.*: */语句覆盖率 (Line Coverage)  : /;
            s/functions\.*: */函数覆盖率 (Func Coverage)  : /;
            s/branches\.*: */分支覆盖率 (Branch Coverage): /'

echo ""
echo "────────────────────────────────────────────────────────────"
echo "  HTML 报告入口"
echo "────────────────────────────────────────────────────────────"
echo "  · 总览（含 Line + Branch 双栏显示）："
echo "    file://$SCRIPT_DIR/$HTML_DIR/index.html"
echo "  · module_driver.c 明细（Line/Branch 分栏 + 每行颜色高亮）："
echo "    file://$SCRIPT_DIR/$HTML_DIR/driver_only/index.html"
echo ""

# -------- 校验目标是否达成 100% Line + 100% Branch --------
title "覆盖率目标校验（module_driver.c：语句 100% 且 分支 100%）"

# lcov --summary 输出样例：
#   lines......: 100.0% (103 of 103 lines)
#   functions..: 100.0% (18 of 18 functions)
#   branches...: 100.0% (39 of 39 branches)
DRIVER_SUMMARY=$(lcov --summary "$DRIVER_INFO" --rc branch_coverage=1 2>&1)

LINE_RATE=$(echo "$DRIVER_SUMMARY" | sed -nE 's/.*lines\.*: +([0-9.]+)%.*/\1/p')
LINE_HIT=$(echo "$DRIVER_SUMMARY"  | sed -nE 's/.*lines.*\(([0-9]+) of ([0-9]+) lines\).*/\1\/\2/p')
FUNC_RATE=$(echo "$DRIVER_SUMMARY" | sed -nE 's/.*functions\.*: +([0-9.]+)%.*/\1/p')
BRANCH_RATE=$(echo "$DRIVER_SUMMARY" | sed -nE 's/.*branches\.*: +([0-9.]+)%.*/\1/p')
BRANCH_HIT=$(echo "$DRIVER_SUMMARY" | sed -nE 's/.*branches.*\(([0-9]+) of ([0-9]+) branches\).*/\1\/\2/p')

echo "  module_driver.c 语句覆盖率 (Line)   = ${LINE_RATE}%  (${LINE_HIT:-N/A})"
echo "  module_driver.c 函数覆盖率 (Func)   = ${FUNC_RATE}%"
echo "  module_driver.c 分支覆盖率 (Branch) = ${BRANCH_RATE}%  (${BRANCH_HIT:-N/A})"

TARGET_OK=1
case "${LINE_RATE:-0}" in
    100|100.0) ;;
    *) err "未达标：语句覆盖率 ${LINE_RATE:-0}% ≠ 100%"; TARGET_OK=0 ;;
esac
case "${BRANCH_RATE:-0}" in
    100|100.0) ;;
    *) err "未达标：分支覆盖率 ${BRANCH_RATE:-0}% ≠ 100%"; TARGET_OK=0 ;;
esac

if [ "$TARGET_OK" = "1" ]; then
    echo ""
    echo "${GREEN}${BOLD}✔ 达标：module_driver.c 语句覆盖率 100%，分支覆盖率 100%（两者分开显示于 HTML 报告）${RESET}"
    exit 0
else
    echo ""
    warn "覆盖率未达到双 100%，请打开 HTML 报告定位未覆盖的语句/分支。"
    exit 3
fi
