/**
 * @file lv_conf.h
 * rig-lookout 模拟器的 LVGL 9.3.0 配置。
 *
 * 真源：codex-desk-terminal/simulator/lv_conf.h（拷贝改造，两端锁定 9.3.0，
 * commit c033a98——vendor/lvgl 同一份）。与 firmware/components/rk_lvgl/lv_conf.h
 * 除宿主差异项（SDL/esp_timer）外逐宏一致。
 *
 * - LV_COLOR_DEPTH = 1：400x300 单色是项目约束（PLAN §5）。
 * - 400x300 I1：LVGL 9.3 SDL 驱动要求 LV_SDL_RENDER_MODE = PARTIAL。
 * - 未定义项回落 vendor/lvgl/src/lv_conf_internal.h 默认值。
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   颜色 / 内存 / 时基
 *===================*/
#define LV_COLOR_DEPTH 1

#define LV_MEM_SIZE (128 * 1024U)          /* 内置分配器池（与固件端同值） */
#define LV_MEM_ADR 0

#define LV_USE_OS LV_OS_NONE               /* 单线程主循环 */

/*====================
   SDL 宿主（软件渲染；SDL_VIDEODRIVER=dummy 可无头出帧）
 *===================*/
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL  /* 深度 1 下唯一合法取值 */
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_ACCELERATED 0               /* 强制软件渲染 */
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 1
#define LV_SDL_MOUSEWHEEL_MODE LV_SDL_MOUSEWHEEL_MODE_ENCODER

#define LV_USE_DRAW_SDL 0

/*====================
   精简功能面（与源项目同面）
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
   主题 / 字体 / 布局
 *===================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_THEME_MONO 0
#define LV_USE_THEME_SIMPLE 0

/* 全量 unifont 1bpp（1.7MB 位图）> 2^20 → 必须 LARGE（bitmap_index uint32_t），
 * 两端一致（源项目 P4.3 纪律）。 */
#define LV_FONT_FMT_TXT_LARGE 1

#define LV_FONT_MONTSERRAT_14 1            /* LV_FONT_DEFAULT 所指默认字体（源项目同款） */
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_28 1            /* 警报牌标题（f_med 槽） */
#define LV_FONT_MONTSERRAT_48 1            /* 首页超大温度一瞥层（f_big 槽） */

#define LV_USE_FLEX 1                      /* vg_lite 编译单元引用 list/menu 头；源项目同款 */
#define LV_USE_GRID 0

#endif /*LV_CONF_H*/
