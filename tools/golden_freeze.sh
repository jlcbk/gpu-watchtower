#!/bin/sh
# golden_freeze.sh — golden 基线冻结/重生成（P2b）。人工守卫流程。
#
# 红线（tasks/P2b-golden-regression.md）：golden 永不自动覆盖。
#   - golden/ 已存在且非空时，本脚本默认拒绝执行；
#   - 重生成属于人工决策（UI 有意变更 / 差异已登记裁决）：
#     须先逐帧目检新渲染，再显式 GOLDEN_OVERWRITE=1 重跑本脚本；
#   - 回归跑挂时正确动作是修代码或登记差异，不是重生成基线消除失败。
#
# 用法：
#   tools/golden_freeze.sh                          # 首次冻结（golden/ 为空时）
#   GOLDEN_OVERWRITE=1 tools/golden_freeze.sh      # 人工确认后的重生成
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOLDEN="$ROOT/golden"

if [ -d "$GOLDEN" ] && [ -n "$(ls -A "$GOLDEN" 2>/dev/null)" ] \
   && [ "${GOLDEN_OVERWRITE:-0}" != "1" ]; then
    echo "[freeze] REFUSE: $GOLDEN 已存在且非空 —— golden 永不自动覆盖。" >&2
    echo "[freeze] 重生成流程：①tools/golden_run.sh --render-only /tmp/新基线 逐帧目检" >&2
    echo "[freeze]           ②确认 UI 变更有据后：GOLDEN_OVERWRITE=1 $0" >&2
    exit 1
fi

"$ROOT/tools/golden_run.sh" --render-only "$GOLDEN"

echo "[freeze] 已渲染 $(ls "$GOLDEN"/*.png | wc -l | tr -d ' ') 帧 → $GOLDEN"
echo "[freeze] 出库前请逐帧目检（Read 打开 PNG），并补 golden/README.md 帧清单核对。"
