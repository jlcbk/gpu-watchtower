# rig-lookout 模拟器工程（P2a）。
#
# 独立 CMake 工程：vendor/lvgl（锁 commit c033a98，LVGL 9.3.0，拷贝自源项目
# 同一 tarball）+ third_party/sdl2-install（SDL2 release-2.30.12 安装树拷贝）。
# shared/ 源码以相对路径引用（不复制）：与 firmware/main 编译同一份 UI/解析。
cmake_minimum_required(VERSION 3.16)
project(rig-lookout-sim LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(SIM_PROJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(LVGL_DIR "${SIM_PROJ_ROOT}/vendor/lvgl")
set(SDL2_PREFIX "${SIM_PROJ_ROOT}/third_party/sdl2-install")

if(NOT EXISTS "${LVGL_DIR}/lv_version.h")
    message(FATAL_ERROR "vendor/lvgl 不存在（应含 commit c033a98 / LVGL 9.3.0）")
endif()
if(NOT EXISTS "${SDL2_PREFIX}/lib/cmake/SDL2/SDL2Config.cmake")
    message(FATAL_ERROR "SDL2 未安装（third_party/sdl2-install 缺失）")
endif()

# --- LVGL 集成（os_desktop.cmake 路径）---
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH
    "rig-lookout-sim lv_conf.h" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

add_subdirectory("${LVGL_DIR}" "${CMAKE_BINARY_DIR}/lvgl")

# --- SDL2（静态库安装树）---
list(APPEND CMAKE_PREFIX_PATH "${SDL2_PREFIX}")
find_package(SDL2 CONFIG REQUIRED)
target_include_directories(lvgl PRIVATE "${SDL2_PREFIX}/include")

add_executable(rig-lookout-sim
    main.c
    # shared/（双端同一份；固件侧 firmware/main/CMakeLists.txt 编译相同清单）
    "${SIM_PROJ_ROOT}/shared/display/rk_frame.c"
    "${SIM_PROJ_ROOT}/shared/stats/rk_json.c"
    "${SIM_PROJ_ROOT}/shared/stats/rk_stats.c"
    "${SIM_PROJ_ROOT}/shared/ui/rk_ui.c"
    "${SIM_PROJ_ROOT}/shared/ui/rk_font_unifont16.c"
)
target_include_directories(rig-lookout-sim PRIVATE
    "${SIM_PROJ_ROOT}/shared/display"
    "${SIM_PROJ_ROOT}/shared/stats"
    "${SIM_PROJ_ROOT}/shared/ui"
)
target_link_libraries(rig-lookout-sim PRIVATE lvgl SDL2::SDL2-static)
target_compile_options(rig-lookout-sim PRIVATE -Wall -Wextra)
