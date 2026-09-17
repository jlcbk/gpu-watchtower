# rig-lookout 主程序（P2a 骨架）。
# shared 源码以相对路径引用编译（与 simulator/CMakeLists 同一清单——双端同一份
# UI/解析）：shared/display: rk_frame.c；shared/stats: rk_json.c rk_stats.c；
# shared/ui: rk_ui.c + rk_font_unifont16.c。
idf_component_register(
    SRCS "main.c"
         "lvgl_port.c"
         "rig_poll.c"
         "rig_wifi.c"
         "rig_batt.c"
         "rig_hist.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../shared/display/rk_frame.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../shared/stats/rk_json.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../shared/stats/rk_stats.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../shared/ui/rk_ui.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../shared/ui/rk_font_unifont16.c"
    INCLUDE_DIRS "."
    REQUIRES rk_lvgl display_st7305 nvs_flash esp_wifi esp_event esp_netif
             esp_timer esp_driver_gpio esp_lcd esp_http_client esp_adc)

# include 面：shared/display（rk_frame.h，display_st7305 已 PUBLIC 传递）、
# shared/stats（rk_stats/rk_json）、shared/ui（rk_ui/rk_font）。
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../shared/display"
    "${CMAKE_CURRENT_LIST_DIR}/../../shared/stats"
    "${CMAKE_CURRENT_LIST_DIR}/../../shared/ui")

# 离线演示构建：idf.py -C firmware build -DRK_DEVNET_OFFLINE=1
# （不编入任何凭据；运行时链路如实显示「主机离线」。红线：默认凭据绝不编译。）
set(RK_DEVNET_OFFLINE "" CACHE STRING "离线演示构建（1=无网络凭据，OFFLINE 态）")
if(RK_DEVNET_OFFLINE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RK_DEVNET_OFFLINE=1)
endif()
