#!/bin/bash
# fetch_vendor.sh — 拉齐不入库的构建依赖（vendor/ 与 third_party 见 .gitignore）
# LVGL 锁定 commit c033a98（与 golden 基线/双端编译一致性绑定，勿改）
set -e
cd "$(dirname "$0")/.."

if [ ! -d vendor/lvgl ]; then
    git clone https://github.com/lvgl/lvgl.git vendor/lvgl
fi
git -C vendor/lvgl checkout c033a98
echo "vendor/lvgl ready @ c033a98"

# 模拟器另需 SDL2 2.30.12 安装树（third_party/sdl2-install）：
#   curl -LO https://libsdl.org/release/SDL2-2.30.12.tar.gz && tar xzf SDL2-2.30.12.tar.gz
#   cmake -S SDL2-2.30.12 -B /tmp/sdl-build -DCMAKE_INSTALL_PREFIX="$PWD/third_party/sdl2-install" \
#     && cmake --build /tmp/sdl-build --target install
echo "done."
