# rig-lookout 固件（P2a 骨架；ESP-IDF v5.5.5，target esp32s3）。
# 版本口径沿用源项目（codex-desk-terminal docs/VERSIONS.md：IDF v5.5.5 tag ff1bac0）。
# LVGL 9.3.0 真源：vendor/lvgl（锁 commit c033a98，经 components/rk_lvgl 接入）。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rk_device)
