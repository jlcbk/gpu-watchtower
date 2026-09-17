# ST7305 驱动组件（P2a；拷贝自 codex-desk-terminal 同名组件，上游只读）。
# shared/display 以相对路径引用（rk_frame.h：400x300 1bpp 逻辑帧）。
idf_component_register(
    SRCS "st7305.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_gpio)

target_include_directories(${COMPONENT_LIB} PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/display")
