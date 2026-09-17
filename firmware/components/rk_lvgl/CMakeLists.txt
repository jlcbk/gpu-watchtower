# rk_lvgl — LVGL 9.3.0 组件化接入（P2a）。
#
# 拷贝自 codex-desk-terminal firmware/components/cdt_lvgl（上游只读）。
# 源码真源：vendor/lvgl（锁 commit c033a98；与 simulator 同一份 vendor）。
# file(GLOB_RECURSE) 全量 src/*.c/*.S——被 lv_conf 关闭的可选功能在源文件
# 内部由 #if 编译为空。
#
# 配置注入：LV_KCONFIG_IGNORE + LV_CONF_PATH 指向本组件 lv_conf.h。
# 两宏必须 PUBLIC——shared/ui 引用方 include lvgl.h 时走同一份 lv_conf.h。
set(LVGL_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../vendor/lvgl")

if(NOT EXISTS "${LVGL_ROOT_DIR}/lv_version.h")
    message(FATAL_ERROR "vendor/lvgl 不存在或无效（应含 LVGL 9.3.0 commit c033a98）")
endif()

file(GLOB_RECURSE LVGL_SOURCES
    "${LVGL_ROOT_DIR}/src/*.c"
    "${LVGL_ROOT_DIR}/src/*.S")

idf_component_register(
    SRCS ${LVGL_SOURCES}
    INCLUDE_DIRS "${LVGL_ROOT_DIR}"
    REQUIRES esp_timer)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    LV_KCONFIG_IGNORE
    LV_CONF_PATH="${CMAKE_CURRENT_LIST_DIR}/lv_conf.h")
