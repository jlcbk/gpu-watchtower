#!/bin/sh
# export_p2a.sh — P2a 八帧导出（2 页 × 4 态 → artifacts/p2a/*.png）。
#
# 无头（SDL_VIDEODRIVER=dummy）；每帧一次模拟器进程，确定性 PNG。
# 用法：tools/export_p2a.sh            （先构建：cmake -S simulator -B build/simulator …）
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/simulator/rig-lookout-sim"
OUT="$ROOT/artifacts/p2a"
PROTO="$ROOT/protocol"

[ -x "$BIN" ] || { echo "[export] ERROR: $BIN 不存在，请先构建模拟器" >&2; exit 1; }
mkdir -p "$OUT"

# 公共参数：时钟/最后在线固定（确定性）；走势种子固定
COMMON="--clock 21:04 --trend-seed 42"

export SDL_VIDEODRIVER=dummy

frame() { # name, fixture, state, page, extra...
  name="$1"; fixture="$2"; state="$3"; page="$4"; shift 4
  echo "[export] $name"
  "$BIN" --fixture "$fixture" --state "$state" --page "$page" \
         --png "$OUT/$name.png" $COMMON "$@" >/dev/null
}

frame page1-normal   "$PROTO/stats.normal.json"   normal   1
frame page1-offline  "$PROTO/stats.normal.json"   offline  1
frame page1-nodriver "$PROTO/stats.nodriver.json" nodriver 1
frame page1-alarm    "$PROTO/stats.normal.json"   alarm    1 --alarm-temp 87 --alarm-src gpu
frame page2-normal   "$PROTO/stats.normal.json"   normal   2
frame page2-offline  "$PROTO/stats.normal.json"   offline  2
frame page2-nodriver "$PROTO/stats.nodriver.json" nodriver 2
frame page2-alarm    "$PROTO/stats.normal.json"   alarm    2 --alarm-temp 87 --alarm-src gpu

ls -l "$OUT"
echo "[export] OK: 8 frames -> $OUT/"
