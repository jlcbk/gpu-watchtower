/**
 * @file lv_conf.h
 * rig-lookout 固件（P2a）的 LVGL 9.3.0 配置。
 *
 * 真源：simulator/lv_conf.h（拷贝改造；除宿主差异项 SDL→无、tick→esp_timer
 * 外逐宏一致——渲染面与模拟器对齐，golden 像素回归前提，P2b 启用）。
 *
 * - LV_COLOR_DEPTH = 1：400x300 单色（PLAN §5）。
 *   LVGL I1 位语义 1=白/0=黑；rig-lookout 逻辑帧 1=黑/0=白，由 lvgl_port
 *   的 flush_cb 按行取反转换（源项目 P1.3 同款）。
 * - 未定义项回落 vendor/lvgl/src/lv_conf_internal.h 默认值（与模拟器同
 *   vendor 同默认，LV_DRAW_BUF_STRIDE_ALIGN=1 → I1@400 行距 50 字节 =
 *   rk_frame_t 布局）。
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   颜色 / 内存 / 时基
 *===================*/
#define LV_COLOR_DEPTH 1

#define LV_MEM_SIZE (128 * 1024U)          /* 内置分配器池（与模拟器同值） */
#define LV_MEM_ADR 0

#define LV_USE_OS LV_OS_NONE               /* 单任务模型：UI 对象只在 ui 任务操作 */

/* 固件时基：esp_timer 毫秒（替代模拟器的 SDL_GetTicks；无 tick 任务）。 */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR ((uint32_t)(esp_timer_get_time() / 1000LL))

/*====================
   宿主设备驱动（固件无 SDL/桌面栈）
 *===================*/
#define LV_USE_SDL 0
#define LV_USE_DRAW_SDL 0

/*====================
   精简功能面（与模拟器同面）
 *===================*/
#define LV_USE_LOG 0

#define LV_USE_VECTOR_GRAPHIC 0
#define LV_USE_FFMPEG 0
#define LV_USE_LOTTIE 0
#define LV_USE_SNAPSHOT 0
#define LV_USE_IMGFONT 0
#define LV_USE_GRIDNAV 0
#define LV_USE_SPAN 0

#define LV_USE_LIBPNG 0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_TJPGD 0
#define LV_USE_BMP 0
#define LV_USE_GIF 0
#define LV_USE_QRCODE 0
#define LV_USE_FREETYPE 0
#define LV_USE_TINY_TTF 0

/*====================
   主题 / 字体 / 布局（渲染对齐关键项，禁止单端改动）
 *===================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_THEME_MONO 0
#define LV_USE_THEME_SIMPLE 0

/* 全量 unifont 1bpp（1.7MB 位图）> 2^20 → 必须 LARGE（bitmap_index uint32_t），
 * 两端一致。 */
#define LV_FONT_FMT_TXT_LARGE 1

#define LV_FONT_MONTSERRAT_14 1            /* LV_FONT_DEFAULT 所指默认字体 */
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_28 1            /* 警报牌标题（f_med 槽） */
#define LV_FONT_MONTSERRAT_48 1            /* 首页超大温度一瞥层（f_big 槽） */

#define LV_USE_FLEX 1                      /* vg_lite 编译单元引用 list/menu 头；源项目同款 */
#define LV_USE_GRID 0

#endif /*LV_CONF_H*/
