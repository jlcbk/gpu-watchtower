/*
 * rk_font_unifont16.h — GNU Unifont 1bpp 字体（生成文件，勿手改）
 * 真源、字符集与 fallback 链见 rk_font_unifont16.c 头注释；
 * 契约：docs/VERSIONS.md 字体行、tests/SCENARIOS.md §5.3（不静默缺字）。
 */
#ifndef RK_FONT_UNIFONT16_H
#define RK_FONT_UNIFONT16_H

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 正文/紧凑唯一字体档（activity/attention/plan/usage/项目名/底栏/行列表/提示条） */
extern const lv_font_t rk_font_unifont_16;

/* 码点是否在本字体覆盖范围内（cdt_ui_ascii_safe 放行判断） */
bool rk_font_unifont16_covers(uint32_t codepoint);

#ifdef __cplusplus
}
#endif

#endif /* RK_FONT_UNIFONT16_H */
