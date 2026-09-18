#!/bin/sh
# golden_run.sh — rig-lookout golden 像素回归 runner（P2b）。
#
# 用法：
#   tools/golden_run.sh                  # 全量渲染 14 帧 → 与 golden/ 逐像素比对 → N/N PASS
#   tools/golden_run.sh --selftest       # 注入自检：篡改 golden 副本单像素必须 FAIL 检出
#   tools/golden_run.sh --render-only D  # 只把 14 帧渲染进目录 D（golden_freeze.sh 专用）
#
# 环境变量：GOLDEN_DIR=<dir> 指定基线目录（默认 <repo>/golden；--selftest 内部用副本）。
#
# 红线：本脚本对 golden/ 只读，永不写入/覆盖。写路径只有 golden_freeze.sh 的人工守卫流程。
# 帧清单与确定性参数（--clock 21:04 --trend-seed 42 --batt 4.12V）唯一真源在本文件；
# 改动即「换基线」，必须走 golden_freeze.sh 重新人工质检冻结，不许直接覆盖 golden。
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/simulator/rig-lookout-sim"
PROTO="$ROOT/protocol"
DIFF="$ROOT/tools/golden_diff.py"
GOLDEN="${GOLDEN_DIR:-$ROOT/golden}"

# 确定性公共参数（任务书 P2b §工作范围 3）
COMMON='--clock 21:04 --trend-seed 42 --batt 4.12V --env "24.9C 36%"'

# 帧清单（14）：name|fixture|state|page|extra
#   8 基础 = 2 页 × normal/offline/nodriver/alarm
#   变体   = busy 亮×2 页、batt=USB×2 页、offline 沿用 stale 快照×2 页
#   stale 帧用 stats.live.json + --last-online 20:58，钉死三个行为：
#   离线不接管页面 / 数据沿用最后快照 / 「最后在线」时刻独立于底栏时钟
FRAMES='
page1-normal|stats.normal.json|normal|1|
page2-normal|stats.normal.json|normal|2|
page1-offline|stats.normal.json|offline|1|
page2-offline|stats.normal.json|offline|2|
page1-nodriver|stats.nodriver.json|nodriver|1|
page2-nodriver|stats.nodriver.json|nodriver|2|
page1-alarm|stats.normal.json|alarm|1|--alarm-temp 87 --alarm-src gpu
page2-alarm|stats.normal.json|alarm|2|--alarm-temp 87 --alarm-src gpu
page1-busy|stats.normal.json|normal|1|--busy
page2-busy|stats.normal.json|normal|2|--busy
page1-batt-usb|stats.normal.json|normal|1|--batt USB
page2-batt-usb|stats.normal.json|normal|2|--batt USB
page1-offline-stale|stats.live.json|offline|1|--last-online 20:58
page2-offline-stale|stats.live.json|offline|2|--last-online 20:58
'

frame_count() { printf '%s' "$FRAMES" | grep -c '^page'; }

render_frames() { # <outdir> — 14 帧确定性渲染
    _out="$1"
    mkdir -p "$_out"
    printf '%s\n' "$FRAMES" | while IFS='|' read -r _name _fx _st _pg _extra; do
        [ -n "$_name" ] || continue
        # COMMON/_extra 是本文件内受控字面量串；eval 展开以支持值内空格
        # （--env "24.9C 36%"），引号在 COMMON 定义处显式给出。
        eval '"$BIN" --fixture "$PROTO/$_fx" --state "$_st" --page "$_pg"' \
               "$COMMON $_extra" --png "$_out/$_name.png" >/dev/null \
            || echo "[render] FAILED: $_name（sim 非零退出）"
        echo "[render] $_name"
    done
}

do_run() { # <workdir> — 渲染进 workdir 并与 $GOLDEN 逐像素比对；全绿返回 0
    _work="$1"
    render_frames "$_work" >/dev/null
    _res="$_work/.results"
    : > "$_res"
    printf '%s\n' "$FRAMES" | while IFS='|' read -r _name _fx _st _pg _extra; do
        [ -n "$_name" ] || continue
        _g="$GOLDEN/$_name.png"
        _f="$_work/$_name.png"
        if [ ! -f "$_g" ]; then
            echo "[MISS] $_name: golden 缺失 $_g"
            echo miss >> "$_res"
            continue
        fi
        if [ ! -f "$_f" ]; then
            echo "[DIFF] $_name: 渲染失败（无 PNG 产出）"
            echo diff >> "$_res"
            continue
        fi
        if _out="$(python3 "$DIFF" cmp "$_g" "$_f" 2>&1)"; then
            echo "[ ok ] $_name"
            echo ok >> "$_res"
        else
            echo "[DIFF] $_name: $_out"
            echo diff >> "$_res"
        fi
    done
    _total=$(frame_count)
    _ok=$(grep -c '^ok$' "$_res" || true)
    _diff=$(grep -c '^diff$' "$_res" || true)
    _miss=$(grep -c '^miss$' "$_res" || true)
    _bad=$((_diff + _miss))
    if [ "$_ok" -eq "$_total" ] && [ "$_bad" -eq 0 ]; then
        echo "${_total}/${_total} PASS"
        return 0
    fi
    echo "${_ok}/${_total} PASS, ${_bad} FAIL（逐帧差异见上：diff_pixels=像素数 first=首差坐标）"
    return 1
}

preflight() {
    [ -x "$BIN" ] || { echo "[run] ERROR: $BIN 不存在，先 cmake --build build/simulator" >&2; return 2; }
    [ -f "$DIFF" ] || { echo "[run] ERROR: $DIFF 缺失" >&2; return 2; }
    return 0
}

cmd_run() {
    preflight || exit $?
    [ -d "$GOLDEN" ] || { echo "[run] ERROR: 基线 $GOLDEN 不存在（先 tools/golden_freeze.sh）" >&2; exit 2; }
    _work="$(mktemp -d /tmp/rk-golden-run.XXXXXX)"
    trap 'rm -rf "$_work"' EXIT INT TERM
    echo "[run] 基线: $GOLDEN"
    do_run "$_work"
}

cmd_selftest() {
    preflight || exit $?
    [ -d "$GOLDEN" ] || { echo "[selftest] ERROR: 基线 $GOLDEN 不存在" >&2; exit 2; }
    _work="$(mktemp -d /tmp/rk-golden-selftest.XXXXXX)"
    trap 'rm -rf "$_work"' EXIT INT TERM

    echo "[selftest] ===== ① 篡改前：对当前代码全量比对（应全绿） ====="
    _pre=0
    do_run "$_work/pre" || _pre=$?
    echo "[selftest] 篡改前 runner 退出码: ${_pre}（0=PASS）"

    echo "[selftest] ===== ② 篡改 golden 副本 page1-normal 单像素 (7,7) ====="
    _copy="$_work/golden-copy"
    mkdir -p "$_copy"
    cp "$GOLDEN"/*.png "$_copy/"
    python3 "$DIFF" flip "$_copy/page1-normal.png" "$_copy/.flip" 7 7
    mv "$_copy/.flip" "$_copy/page1-normal.png"
    echo "[selftest] 副本已注入 1 像素差异（golden 原件未动）"

    echo "[selftest] ===== ③ 篡改后：对副本全量比对（必须 FAIL 检出） ====="
    GOLDEN="$_copy"
    _post=0
    do_run "$_work/post" || _post=$?
    if [ "$_pre" -eq 0 ] && [ "$_post" -ne 0 ]; then
        echo "[selftest] SELFTEST PASS：篡改前 0 退出 → 篡改后非 0，单像素注入被检出"
        exit 0
    fi
    echo "[selftest] SELFTEST FAIL：篡改前码=${_pre} 篡改后码=${_post}（预期 0 / 非0）"
    exit 1
}

cmd_render_only() {
    preflight || exit $?
    render_frames "$1"
}

case "${1:-}" in
    "")            cmd_run ;;
    --selftest)    cmd_selftest ;;
    --render-only) [ -n "${2:-}" ] || { echo "usage: $0 --render-only <dir>" >&2; exit 2; }
                   cmd_render_only "$2" ;;
    *)             echo "usage: $0 [--selftest|--render-only <dir>]" >&2; exit 2 ;;
esac
