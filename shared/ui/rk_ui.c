/*
 * rk_ui.c — rig-lookout 共享 UI 实现（双页 × 四态，PLAN §5）。
 *
 * 结构：三根容器（alarm 抢占 > page1/page2 互斥）。所有 widget 一次性建好，
 * apply 只改文本/尺寸/可见性（无动态分配，适配合入 FreeRTOS 单任务模型）。
 * 1bpp 纪律：一律 remove_style_all 后显式黑白；字体经可写副本挂 unifont16
 * fallback（Montserrat 无 °/CJK/⚠，回退渲染 16px 字形）。
 */
#include "rk_ui.h"

#include <stdio.h>
#include <string.h>

#include "rk_font_unifont16.h"

/* ---------- 布局常量 ---------- */
#define RK_W 400
#define RK_H 300
#define RK_M 8          /* 外边距 */
#define TITLE_Y 8
#define TITLE_H 24
#define BOT_Y 270
#define BOT_H 22
#define CONTENT_Y 36
#define CONTENT_B 264   /* 内容区下缘（底栏分隔线之上） */
#define COL2_X 206      /* 页 1/2 右列 x */
#define COL_W 186       /* 右列宽 */
#define TREND_SEP_Y 196 /* 页 1 走势区分隔线 */
#define TREND_Y 204
#define TREND_H 58

/* ---------- 字体槽（rk_ui_init 内挂 fallback） ---------- */
#define F_BODY (&rk_font_unifont_16)
static lv_font_t f_big;   /* montserrat48 副本 + unifont fallback：超大温度 */
static lv_font_t f_med;   /* montserrat28 副本 + unifont fallback：警报标题 */

/* ---------- 内部小工具 ---------- */

/* remove_style_all + 定位/尺寸（布局原子，沿用源项目约定） */
static void ui_box(lv_obj_t *o, int x, int y, int wd, int ht)
{
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, (int32_t)x, (int32_t)y);
    lv_obj_set_size(o, (int32_t)wd, (int32_t)ht);
}

/* 黑字 label 一体化 */
static lv_obj_t *ui_text(lv_obj_t *parent, const lv_font_t *font, int x, int y,
                         int wd, int ht)
{
    lv_obj_t *l = lv_label_create(parent);
    ui_box(l, x, y, wd > 0 ? wd : LV_SIZE_CONTENT, ht > 0 ? ht : LV_SIZE_CONTENT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(l, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    if (wd > 0) lv_obj_set_width(l, (int32_t)wd);
    return l;
}

/* 反白 label（黑底白字，须配 ui_box 的父容器） */
static lv_obj_t *ui_text_inv(lv_obj_t *parent, const lv_font_t *font, int x, int y,
                             int wd, int ht)
{
    lv_obj_t *l = ui_text(parent, font, x, y, wd, ht);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    return l;
}

/* 反白小牌（黑底白字，通用：渲染中标签等） */
static lv_obj_t *ui_inv_tag(lv_obj_t *parent, int x, int y, int wd, lv_obj_t **label_out)
{
    lv_obj_t *box = lv_obj_create(parent);
    ui_box(box, x, y, wd, 18);
    lv_obj_set_style_bg_color(box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    *label_out = ui_text_inv(box, F_BODY, 0, 1, wd - 2, 16);
    lv_obj_set_style_text_align(*label_out, LV_TEXT_ALIGN_CENTER, 0);
    return box;
}

/* WiFi 反白小标记（黑底白字；wl* 链路提醒，PLAN §5） */
static lv_obj_t *ui_wifi_tag(lv_obj_t *parent, lv_obj_t **label_out)
{
    lv_obj_t *box = lv_obj_create(parent);
    ui_box(box, 336, TITLE_Y + 3, 56, 18);
    lv_obj_set_style_bg_color(box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    *label_out = ui_text_inv(box, F_BODY, 0, 1, 54, 16);
    lv_obj_set_style_text_align(*label_out, LV_TEXT_ALIGN_CENTER, 0);
    return box;
}

/* 横条：白底黑框槽 + 黑色填充子块。返回槽容器（隐藏槽 = 整条隐藏）；
 * bar_set 经子对象改填充宽（fill 是 slot 的第 0 个子对象）。 */
static lv_obj_t *ui_bar(lv_obj_t *parent, int x, int y, int wd, int ht)
{
    lv_obj_t *slot = lv_obj_create(parent);
    ui_box(slot, x, y, wd, ht);
    lv_obj_set_style_bg_color(slot, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(slot, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(slot, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(slot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fill = lv_obj_create(slot);
    ui_box(fill, 0, 0, 0, ht - 2);
    lv_obj_set_style_bg_color(fill, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(fill, 0, LV_PART_MAIN);
    lv_obj_set_height(fill, (int32_t)(ht - 2));
    return slot;
}

static void bar_set(lv_obj_t *slot, int wd_slot, double pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int w = (int)((double)(wd_slot - 2) * pct / 100.0 + 0.5);
    lv_obj_t *fill = lv_obj_get_child(slot, 0);
    if (fill != NULL) lv_obj_set_width(fill, (int32_t)w);
}

/* 数值 → 文本：缺项显 "--"（PLAN §4 铁律） */
static void fmt1(char *buf, size_t cap, const rk_num_t *n, const char *fmt)
{
    if (n == NULL || !n->present) {
        snprintf(buf, cap, "--");
    } else {
        snprintf(buf, cap, fmt, n->value);
    }
}

static void set_txt(lv_obj_t *l, const char *txt)
{
    lv_label_set_text(l, txt != NULL ? txt : "--");
}

static void set_visible(lv_obj_t *o, bool vis)
{
    if (vis) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- widget 句柄 ---------- */

typedef struct {
    /* 标题（两页共用布局，各页一份） */
    lv_obj_t *p1_root, *p2_root, *alarm_root;
    lv_obj_t *p1_title, *p1_batt, *p1_wifi;
    lv_obj_t *p1_busy, *p1_busy_l;
    lv_obj_t *p1_env;
    lv_obj_t *p1_temp_bg;   /* ≥80°C 反白小块（黑底容器） */
    lv_obj_t *p1_temp;      /* 超大温度（在 temp_bg 内） */
    lv_obj_t *p1_gpuname;
    lv_obj_t *p1_util_l, *p1_vram_l, *p1_pwr_l, *p1_fan_l;
    lv_obj_t *p1_util_b, *p1_vram_b, *p1_pwr_b, *p1_fan_b;
    lv_obj_t *p1_trend_caption;
    lv_obj_t *p1_trend_line;
    lv_obj_t *p1_trend_tick;  /* 75°C 虚线 */
    lv_obj_t *p1_trend_tick_l;
    lv_obj_t *p1_nodriver_box, *p1_nodriver_l;
    lv_obj_t *p1_offtag, *p1_offtag_l; /* 离线小标（底栏，黑底白字；页面不接管） */
    lv_obj_t *p1_pageind, *p1_clock;

    lv_obj_t *p2_title, *p2_batt, *p2_wifi;
    lv_obj_t *p2_busy, *p2_busy_l;
    lv_obj_t *p2_env;
    lv_obj_t *p2_cpu_l;
    lv_obj_t *p2_cpu_b;
    lv_obj_t *p2_ram_l, *p2_ram_b, *p2_swap_l, *p2_swap_b;
    lv_obj_t *p2_net_l, *p2_load_l, *p2_up_l;
    lv_obj_t *p2_d0_l, *p2_d1_l; /* 盘只显数值行（2026-09-17 用户定稿：不用条） */
    lv_obj_t *p2_offtag, *p2_offtag_l;
    lv_obj_t *p2_pageind, *p2_clock;

    lv_obj_t *a_head, *a_temp, *a_note, *a_clock;
    lv_obj_t *sleep_root, *sleep_l; /* 终端屏：低压深睡「休眠中」 */
} rk_ui_t;

static rk_ui_t ui;
static lv_point_precise_t g_trend_pts[120]; /* 走势点存储（≤120 点） */
static lv_point_precise_t g_tick_pts[2];

/* 温度 y 映射：20..95°C → TREND_Y..TREND_Y+TREND_H（低温在下）。
 * 下限 20（2026-09-17 用户需求）：GPU 双态分布（空载 ~25-35 / 满载 ~70-78）
 * 都拿到可见分辨率，空载段不再压底线；75°C 刻度含义不变。 */
static int trend_y(double temp_c)
{
    double t = temp_c;
    if (t < 20.0) t = 20.0;
    if (t > 95.0) t = 95.0;
    double frac = (t - 20.0) / 75.0;
    return TREND_Y + TREND_H - 2 - (int)(frac * (double)(TREND_H - 6) + 0.5);
}

/* ---------- 页 1 ---------- */

static void page1_create(lv_obj_t *root)
{
    ui.p1_title = ui_text(root, F_BODY, RK_M, TITLE_Y, 170, TITLE_H);
    set_txt(ui.p1_title, "LOOKOUT - GPU");
    /* 渲染中状态牌（标题行反白；空载不显示，画面保持干净） */
    ui.p1_busy = ui_inv_tag(root, 270, TITLE_Y + 3, 60, &ui.p1_busy_l);
    set_txt(ui.p1_busy_l, "渲染中");
    ui_wifi_tag(root, &ui.p1_wifi);
    set_txt(ui.p1_wifi, "WiFi");

    /* 超大温度一瞥层（≥80°C 时容器转黑底反白） */
    ui.p1_temp_bg = lv_obj_create(root);
    ui_box(ui.p1_temp_bg, RK_M, 44, 196, 64);
    lv_obj_set_style_pad_all(ui.p1_temp_bg, 0, LV_PART_MAIN);
    ui.p1_temp = ui_text(ui.p1_temp_bg, &f_big, 4, 0, 188, 64);
    /* 显卡名：自动换行两行（2026-09-17 用户需求：单行截断放不下） */
    ui.p1_gpuname = ui_text(root, F_BODY, RK_M + 2, 112, 188, 40);
    lv_label_set_long_mode(ui.p1_gpuname, LV_LABEL_LONG_WRAP);

    /* 右列：四行 label + 条（2026-09-17 用户需求：整体上移一块） */
    int ys[4] = { 40, 76, 112, 148 };
    ui.p1_util_l = ui_text(root, F_BODY, COL2_X, ys[0], COL_W, 16);
    ui.p1_vram_l = ui_text(root, F_BODY, COL2_X, ys[1], COL_W, 16);
    ui.p1_pwr_l = ui_text(root, F_BODY, COL2_X, ys[2], COL_W, 16);
    ui.p1_fan_l = ui_text(root, F_BODY, COL2_X, ys[3], COL_W, 16);
    ui.p1_util_b = ui_bar(root, COL2_X, ys[0] + 18, COL_W, 6);
    ui.p1_vram_b = ui_bar(root, COL2_X, ys[1] + 18, COL_W, 6);
    ui.p1_pwr_b = ui_bar(root, COL2_X, ys[2] + 18, COL_W, 6);
    ui.p1_fan_b = ui_bar(root, COL2_X, ys[3] + 18, COL_W, 6);

    /* 驱动未装态：右列整体让位给警示框 */
    ui.p1_nodriver_box = lv_obj_create(root);
    ui_box(ui.p1_nodriver_box, COL2_X, 60, COL_W, 110);
    lv_obj_set_style_bg_color(ui.p1_nodriver_box, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.p1_nodriver_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui.p1_nodriver_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.p1_nodriver_box, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.p1_nodriver_box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ui.p1_nodriver_box, LV_OBJ_FLAG_SCROLLABLE);
    ui.p1_nodriver_l = ui_text(ui.p1_nodriver_box, F_BODY, 0, 44, COL_W - 4, 18);
    lv_obj_set_style_text_align(ui.p1_nodriver_l, LV_TEXT_ALIGN_CENTER, 0);
    set_txt(ui.p1_nodriver_l, "驱动未装");

    /* 走势区：分隔线 + 折线 + 75°C 虚线刻度 */
    {
        static lv_point_precise_t sep[2] = {{RK_M, TREND_SEP_Y}, {RK_W - RK_M, TREND_SEP_Y}};
        lv_obj_t *sep_line = lv_line_create(root);
        lv_line_set_points(sep_line, sep, 2);
        lv_obj_set_style_line_color(sep_line, lv_color_black(), 0);
        lv_obj_set_style_line_width(sep_line, 1, 0);
    }
    ui.p1_trend_caption = ui_text(root, F_BODY, RK_M, TREND_SEP_Y + 1, 220, 16);
    set_txt(ui.p1_trend_caption, "TREND 60MIN");
    ui.p1_trend_line = lv_line_create(root);
    lv_obj_set_style_line_color(ui.p1_trend_line, lv_color_black(), 0);
    lv_obj_set_style_line_width(ui.p1_trend_line, 1, 0);
    ui.p1_trend_tick = lv_line_create(root);
    lv_obj_set_style_line_color(ui.p1_trend_tick, lv_color_black(), 0);
    lv_obj_set_style_line_width(ui.p1_trend_tick, 1, 0);
    lv_obj_set_style_line_dash_width(ui.p1_trend_tick, 2, 0);
    lv_obj_set_style_line_dash_gap(ui.p1_trend_tick, 3, 0);
    ui.p1_trend_tick_l = ui_text(root, F_BODY, RK_W - RK_M - 58, TREND_SEP_Y + 1, 58, 16);
    lv_obj_set_style_text_align(ui.p1_trend_tick_l, LV_TEXT_ALIGN_RIGHT, 0);

    /* 离线小标（底栏反白小块；离线不接管页面，数据沿用上一份好快照） */
    ui.p1_offtag = lv_obj_create(root);
    ui_box(ui.p1_offtag, 124, BOT_Y + 2, 56, 18);
    lv_obj_set_style_bg_color(ui.p1_offtag, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.p1_offtag, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.p1_offtag, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.p1_offtag, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.p1_offtag, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ui.p1_offtag, LV_OBJ_FLAG_SCROLLABLE);
    ui.p1_offtag_l = ui_text_inv(ui.p1_offtag, F_BODY, 0, 1, 54, 16);
    lv_obj_set_style_text_align(ui.p1_offtag_l, LV_TEXT_ALIGN_CENTER, 0);
    set_txt(ui.p1_offtag_l, "离线");

    /* 底栏 */
    {
        static lv_point_precise_t sep[2] = {{RK_M, BOT_Y - 1}, {RK_W - RK_M, BOT_Y - 1}};
        lv_obj_t *sep_line = lv_line_create(root);
        lv_line_set_points(sep_line, sep, 2);
        lv_obj_set_style_line_color(sep_line, lv_color_black(), 0);
        lv_obj_set_style_line_width(sep_line, 1, 0);
    }
    ui.p1_pageind = ui_text(root, F_BODY, RK_M, BOT_Y, 88, BOT_H);
    /* 底栏环境位（板载温湿度；离线时让位给「离线」小标） */
    ui.p1_env = ui_text(root, F_BODY, 100, BOT_Y, 104, BOT_H);
    /* 底栏电池位（2026-09-17 用户定稿：与时钟同排） */
    ui.p1_batt = ui_text(root, F_BODY, 214, BOT_Y, 82, BOT_H);
    lv_obj_set_style_text_align(ui.p1_batt, LV_TEXT_ALIGN_RIGHT, 0);
    ui.p1_clock = ui_text(root, F_BODY, RK_W - RK_M - 90, BOT_Y, 90, BOT_H);
    lv_obj_set_style_text_align(ui.p1_clock, LV_TEXT_ALIGN_RIGHT, 0);
}

static void page1_apply(const rk_ui_model_t *m)
{
    const rk_stats_t *s = &m->stats;
    bool nodrv = (m->state == RK_STATE_NODRIVER) ||
                 (m->have_snapshot && s->gpu.driver_present && !s->gpu.driver);
    char buf[64];

    set_txt(ui.p1_title, "LOOKOUT - GPU");
    set_txt(ui.p1_batt, (m->batt_text != NULL && m->batt_text[0] != '\0') ? m->batt_text : "");
    set_visible(ui.p1_busy, m->gpu_busy);
    set_visible(ui.p1_wifi, rk_netif_is_wifi(s->net_if));

    /* 温度一瞥层：≥80°C 反白加重。离线不接管：s 保有上一份好快照照常显示 */
    bool have_temp = !nodrv && s->gpu.temp_c.present;
    double temp = have_temp ? s->gpu.temp_c.value : 0.0;
    bool heavy = have_temp && temp >= RK_TEMP_HEAVY_C;
    lv_obj_set_style_bg_color(ui.p1_temp_bg,
                              heavy ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.p1_temp_bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui.p1_temp,
                                heavy ? lv_color_white() : lv_color_black(), LV_PART_MAIN);
    if (have_temp) snprintf(buf, sizeof(buf), "%.0f°C", temp);
    else snprintf(buf, sizeof(buf), "--");
    set_txt(ui.p1_temp, buf);
    set_txt(ui.p1_gpuname, s->gpu.name[0] != '\0' ? s->gpu.name : "--");

    /* 右列（驱动未装：条槽整体隐藏让位警示框） */
    bool show_bars = !nodrv;
    set_visible(ui.p1_nodriver_box, nodrv);
    set_visible(ui.p1_util_l, !nodrv);
    set_visible(ui.p1_vram_l, !nodrv);
    set_visible(ui.p1_pwr_l, !nodrv);
    set_visible(ui.p1_fan_l, !nodrv);
    set_visible(ui.p1_util_b, show_bars);
    set_visible(ui.p1_vram_b, show_bars);
    set_visible(ui.p1_pwr_b, show_bars);
    set_visible(ui.p1_fan_b, show_bars);

    if (!nodrv) {
        /* 显示层量化（util 5% / pwr 5W 档）：满载时秒级抖动不再连跳推帧 */
        double util = s->gpu.util_pct.present ? ((int)(s->gpu.util_pct.value / 5.0)) * 5.0 : 0.0;
        double pwr = s->gpu.power_w.present ? ((int)(s->gpu.power_w.value / 5.0)) * 5.0 : 0.0;

        if (s->gpu.util_pct.present) snprintf(buf, sizeof(buf), "UTIL %.0f%%", util);
        else snprintf(buf, sizeof(buf), "UTIL --");
        set_txt(ui.p1_util_l, buf);
        bar_set(ui.p1_util_b, COL_W, s->gpu.util_pct.present ? util : 0.0);

        if (s->gpu.vram_used_gb.present && s->gpu.vram_total_gb.present)
            snprintf(buf, sizeof(buf), "VRAM %.1f/%.0fG", s->gpu.vram_used_gb.value, s->gpu.vram_total_gb.value);
        else snprintf(buf, sizeof(buf), "VRAM --");
        set_txt(ui.p1_vram_l, buf);
        bar_set(ui.p1_vram_b, COL_W,
                (s->gpu.vram_used_gb.present && s->gpu.vram_total_gb.present && s->gpu.vram_total_gb.value > 0)
                    ? s->gpu.vram_used_gb.value / s->gpu.vram_total_gb.value * 100.0 : 0.0);

        if (s->gpu.power_w.present && s->gpu.power_limit_w.present)
            snprintf(buf, sizeof(buf), "PWR %.0f/%.0fW", pwr, s->gpu.power_limit_w.value);
        else snprintf(buf, sizeof(buf), "PWR --");
        set_txt(ui.p1_pwr_l, buf);
        bar_set(ui.p1_pwr_b, COL_W,
                (s->gpu.power_w.present && s->gpu.power_limit_w.present && s->gpu.power_limit_w.value > 0)
                    ? pwr / s->gpu.power_limit_w.value * 100.0 : 0.0);

        if (s->gpu.fan_pct.present) snprintf(buf, sizeof(buf), "FAN %.0f%%", s->gpu.fan_pct.value);
        else snprintf(buf, sizeof(buf), "FAN --");
        set_txt(ui.p1_fan_l, buf);
        bar_set(ui.p1_fan_b, COL_W, s->gpu.fan_pct.present ? s->gpu.fan_pct.value : 0.0);
    }

    /* 走势线（驱动缺失时隐藏数据线；刻度线保留） */
    set_visible(ui.p1_trend_line, !nodrv && m->trend != NULL && m->trend_len > 1);
    if (!nodrv && m->trend != NULL && m->trend_len > 1) {
        int n = m->trend_len > 120 ? 120 : m->trend_len;
        for (int i = 0; i < n; i++) {
            g_trend_pts[i].x = (int32_t)(RK_M + 2 + (RK_W - 2 * RK_M - 4) * i / (n - 1));
            g_trend_pts[i].y = (int32_t)trend_y(m->trend[i]);
        }
        lv_line_set_points(ui.p1_trend_line, g_trend_pts, (uint32_t)n);
    }
    {
        int y = trend_y(RK_TEMP_TICK_C);
        g_tick_pts[0].x = RK_M + 2;
        g_tick_pts[0].y = (int32_t)y;
        g_tick_pts[1].x = RK_W - RK_M - 2;
        g_tick_pts[1].y = (int32_t)y;
        lv_line_set_points(ui.p1_trend_tick, g_tick_pts, 2);
    }
    snprintf(buf, sizeof(buf), "75C ---");
    set_txt(ui.p1_trend_tick_l, buf);

    /* 离线小标：只亮底栏黑标，页面与数据不动 */
    set_visible(ui.p1_offtag, m->state == RK_STATE_OFFLINE);

    set_txt(ui.p1_pageind, "P1/2 GPU");
    set_txt(ui.p1_env, (m->env_text != NULL && m->env_text[0] != '\0' && m->state != RK_STATE_OFFLINE) ? m->env_text : "");
    set_txt(ui.p1_clock, (m->clock_text != NULL) ? m->clock_text : "--");
}

/* ---------- 页 2 ---------- */

static void page2_create(lv_obj_t *root)
{
    ui.p2_title = ui_text(root, F_BODY, RK_M, TITLE_Y, 170, TITLE_H);
    set_txt(ui.p2_title, "LOOKOUT - SYS");
    ui.p2_busy = ui_inv_tag(root, 270, TITLE_Y + 3, 60, &ui.p2_busy_l);
    set_txt(ui.p2_busy_l, "渲染中");
    ui_wifi_tag(root, &ui.p2_wifi);
    set_txt(ui.p2_wifi, "WiFi");

    /* 左列：CPU 总占用单条（2026-09-17 用户定稿：不按核展开）+ RAM/SWAP，
     * 三行等距大条，替代旧 6 核小条阵 */
    ui.p2_cpu_l = ui_text(root, F_BODY, RK_M, 44, COL_W, 18);
    ui.p2_cpu_b = ui_bar(root, RK_M, 66, COL_W, 12);
    ui.p2_ram_l = ui_text(root, F_BODY, RK_M, 108, COL_W, 16);
    ui.p2_ram_b = ui_bar(root, RK_M, 130, COL_W, 12);
    ui.p2_swap_l = ui_text(root, F_BODY, RK_M, 172, COL_W, 16);
    ui.p2_swap_b = ui_bar(root, RK_M, 194, COL_W, 12);

    /* 右列：NET/LOAD/UP + 两行盘数值（无条） */
    ui.p2_net_l = ui_text(root, F_BODY, COL2_X, CONTENT_Y, COL_W, 16);
    ui.p2_load_l = ui_text(root, F_BODY, COL2_X, CONTENT_Y + 20, COL_W, 16);
    ui.p2_up_l = ui_text(root, F_BODY, COL2_X, CONTENT_Y + 40, COL_W, 16);
    ui.p2_d0_l = ui_text(root, F_BODY, COL2_X, CONTENT_Y + 68, COL_W, 16);
    ui.p2_d1_l = ui_text(root, F_BODY, COL2_X, CONTENT_Y + 92, COL_W, 16);

    /* 离线小标（底栏反白小块；离线不接管页面） */
    ui.p2_offtag = lv_obj_create(root);
    ui_box(ui.p2_offtag, 124, BOT_Y + 2, 56, 18);
    lv_obj_set_style_bg_color(ui.p2_offtag, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.p2_offtag, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.p2_offtag, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.p2_offtag, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.p2_offtag, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ui.p2_offtag, LV_OBJ_FLAG_SCROLLABLE);
    ui.p2_offtag_l = ui_text_inv(ui.p2_offtag, F_BODY, 0, 1, 54, 16);
    lv_obj_set_style_text_align(ui.p2_offtag_l, LV_TEXT_ALIGN_CENTER, 0);
    set_txt(ui.p2_offtag_l, "离线");

    /* 底栏 */
    {
        static lv_point_precise_t sep[2] = {{RK_M, BOT_Y - 1}, {RK_W - RK_M, BOT_Y - 1}};
        lv_obj_t *sep_line = lv_line_create(root);
        lv_line_set_points(sep_line, sep, 2);
        lv_obj_set_style_line_color(sep_line, lv_color_black(), 0);
        lv_obj_set_style_line_width(sep_line, 1, 0);
    }
    ui.p2_pageind = ui_text(root, F_BODY, RK_M, BOT_Y, 88, BOT_H);
    ui.p2_env = ui_text(root, F_BODY, 100, BOT_Y, 104, BOT_H);
    ui.p2_batt = ui_text(root, F_BODY, 214, BOT_Y, 82, BOT_H);
    lv_obj_set_style_text_align(ui.p2_batt, LV_TEXT_ALIGN_RIGHT, 0);
    ui.p2_clock = ui_text(root, F_BODY, RK_W - RK_M - 90, BOT_Y, 90, BOT_H);
    lv_obj_set_style_text_align(ui.p2_clock, LV_TEXT_ALIGN_RIGHT, 0);
}

static void page2_apply(const rk_ui_model_t *m)
{
    const rk_stats_t *s = &m->stats;
    char buf[64];

    set_txt(ui.p2_title, "LOOKOUT - SYS");
    set_txt(ui.p2_batt, (m->batt_text != NULL && m->batt_text[0] != '\0') ? m->batt_text : "");
    set_visible(ui.p2_busy, m->gpu_busy);
    set_visible(ui.p2_wifi, rk_netif_is_wifi(s->net_if));

    /* CPU 总占用 + 温度（单条，不按核）。离线不接管：沿用上一份好快照 */
    if (s->cpu.temp_c.present && s->cpu.util_pct.present)
        snprintf(buf, sizeof(buf), "CPU %.0fC %.0f%%", s->cpu.temp_c.value, s->cpu.util_pct.value);
    else snprintf(buf, sizeof(buf), "CPU --");
    set_txt(ui.p2_cpu_l, buf);
    bar_set(ui.p2_cpu_b, COL_W, s->cpu.util_pct.present ? s->cpu.util_pct.value : 0.0);

    /* RAM / SWAP */
    if (s->mem.used_gb.present && s->mem.total_gb.present)
        snprintf(buf, sizeof(buf), "RAM %.1f/%.0fG", s->mem.used_gb.value, s->mem.total_gb.value);
    else snprintf(buf, sizeof(buf), "RAM --");
    set_txt(ui.p2_ram_l, buf);
    bar_set(ui.p2_ram_b, COL_W,
            (s->mem.used_gb.present && s->mem.total_gb.present && s->mem.total_gb.value > 0)
                ? s->mem.used_gb.value / s->mem.total_gb.value * 100.0 : 0.0);

    if (s->mem.swap_used_gb.present && s->mem.swap_total_gb.present)
        snprintf(buf, sizeof(buf), "SWAP %.1f/%.0fG", s->mem.swap_used_gb.value, s->mem.swap_total_gb.value);
    else snprintf(buf, sizeof(buf), "SWAP --");
    set_txt(ui.p2_swap_l, buf);
    bar_set(ui.p2_swap_b, COL_W,
            (s->mem.swap_used_gb.present && s->mem.swap_total_gb.present && s->mem.swap_total_gb.value > 0)
                ? s->mem.swap_used_gb.value / s->mem.swap_total_gb.value * 100.0 : 0.0);

    /* 右列 NET/LOAD/UP（NET 取 10K 档量化：网速秒级抖动大，防数字连跳闪屏） */
    if (s->sys.net_rx_kbps.present && s->sys.net_tx_kbps.present)
        snprintf(buf, sizeof(buf), "NET %.0f/%.0fK",
                 (double)(((int)s->sys.net_rx_kbps.value / 10) * 10),
                 (double)(((int)s->sys.net_tx_kbps.value / 10) * 10));
    else snprintf(buf, sizeof(buf), "NET --");
    set_txt(ui.p2_net_l, buf);

    fmt1(buf, sizeof(buf), &s->sys.load1, "LOAD %.1f");
    set_txt(ui.p2_load_l, buf);

    if (m->have_snapshot && s->uptime_present) {
        char upt[24];
        rk_uptime_text(s->uptime_s, upt, sizeof(upt));
        snprintf(buf, sizeof(buf), "UP %s", upt);
    } else {
        snprintf(buf, sizeof(buf), "UP --");
    }
    set_txt(ui.p2_up_l, buf);

    /* 盘：两行数值（不用条） */
    for (int d = 0; d < 2; d++) {
        lv_obj_t *lab = (d == 0) ? ui.p2_d0_l : ui.p2_d1_l;
        bool ok = d < s->sys.disk_count &&
                  s->sys.disks[d].free_gb.present && s->sys.disks[d].total_gb.present;
        if (ok) {
            const char *mnt = s->sys.disks[d].mnt;
            if (strncmp(mnt, "/mnt/", 5) == 0) mnt += 5; /* /mnt/model-library → model-library */
            else if (mnt[0] == '/' && mnt[1] == '\0') mnt = "/";
            snprintf(buf, sizeof(buf), "%s %.0f/%.0fG", mnt,
                     s->sys.disks[d].free_gb.value, s->sys.disks[d].total_gb.value);
            set_txt(lab, buf);
        } else {
            set_txt(lab, "DISK --");
        }
    }

    /* 离线小标：只亮底栏黑标，页面与数据不动 */
    set_visible(ui.p2_offtag, m->state == RK_STATE_OFFLINE);

    set_txt(ui.p2_pageind, "P2/2 SYS");
    set_txt(ui.p2_env, (m->env_text != NULL && m->env_text[0] != '\0' && m->state != RK_STATE_OFFLINE) ? m->env_text : "");
    set_txt(ui.p2_clock, (m->clock_text != NULL) ? m->clock_text : "--");
}

/* ---------- 警报牌（整屏反白，抢占两页） ---------- */

static void alarm_create(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

    /* 内框白边（警报牌视觉） */
    lv_obj_t *frame = lv_obj_create(root);
    ui_box(frame, 4, 4, RK_W - 8, RK_H - 8);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(frame, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(frame, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(frame, 0, LV_PART_MAIN);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    ui.a_head = ui_text_inv(root, &f_med, 0, 44, RK_W, 34);
    lv_obj_set_style_text_align(ui.a_head, LV_TEXT_ALIGN_CENTER, 0);
    ui.a_temp = ui_text_inv(root, &f_big, 0, 100, RK_W, 56);
    lv_obj_set_style_text_align(ui.a_temp, LV_TEXT_ALIGN_CENTER, 0);
    ui.a_note = ui_text_inv(root, F_BODY, 0, 176, RK_W, 18);
    lv_obj_set_style_text_align(ui.a_note, LV_TEXT_ALIGN_CENTER, 0);
    ui.a_clock = ui_text_inv(root, F_BODY, 0, 220, RK_W, 18);
    lv_obj_set_style_text_align(ui.a_clock, LV_TEXT_ALIGN_CENTER, 0);
}

static void alarm_apply(const rk_ui_model_t *m)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "⚠ 超温警报");
    set_txt(ui.a_head, buf);
    snprintf(buf, sizeof(buf), "%s %.0f°C", m->alarm_is_gpu ? "GPU" : "CPU",
             (double)m->alarm_temp_c);
    set_txt(ui.a_temp, buf);
    set_txt(ui.a_note, ">=85C 30S RULE");
    set_txt(ui.a_clock, (m->clock_text != NULL) ? m->clock_text : "--");
}

/* ---------- 入口 ---------- */

void rk_ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* 字体副本 + unifont fallback（Montserrat 缺 °/CJK/⚠ → 回退 16px 字形） */
    f_big = lv_font_montserrat_48;
    f_big.fallback = &rk_font_unifont_16;
    f_med = lv_font_montserrat_28;
    f_med.fallback = &rk_font_unifont_16;

    /* 白底屏幕 */
    lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    memset(&ui, 0, sizeof(ui));
    ui.p1_root = lv_obj_create(scr);
    ui_box(ui.p1_root, 0, 0, RK_W, RK_H);
    lv_obj_set_style_bg_color(ui.p1_root, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.p1_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.p1_root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.p1_root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ui.p1_root, LV_OBJ_FLAG_SCROLLABLE);
    page1_create(ui.p1_root);

    ui.p2_root = lv_obj_create(scr);
    ui_box(ui.p2_root, 0, 0, RK_W, RK_H);
    lv_obj_set_style_bg_color(ui.p2_root, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.p2_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.p2_root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.p2_root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ui.p2_root, LV_OBJ_FLAG_SCROLLABLE);
    page2_create(ui.p2_root);

    ui.alarm_root = lv_obj_create(scr);
    ui_box(ui.alarm_root, 0, 0, RK_W, RK_H);
    lv_obj_set_style_border_width(ui.alarm_root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.alarm_root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ui.alarm_root, LV_OBJ_FLAG_SCROLLABLE);
    alarm_create(ui.alarm_root);

    /* 终端屏（默认隐藏，不影响既有帧；低压深睡时由 rk_ui_sleep_screen 接管） */
    ui.sleep_root = lv_obj_create(scr);
    ui_box(ui.sleep_root, 0, 0, RK_W, RK_H);
    lv_obj_set_style_bg_color(ui.sleep_root, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.sleep_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.sleep_root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.sleep_root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ui.sleep_root, LV_OBJ_FLAG_SCROLLABLE);
    ui.sleep_l = ui_text(ui.sleep_root, &f_med, 0, 132, RK_W, 36);
    lv_obj_set_style_text_align(ui.sleep_l, LV_TEXT_ALIGN_CENTER, 0);
    set_txt(ui.sleep_l, "休眠中");
    set_visible(ui.sleep_root, false);
}

void rk_ui_apply(const rk_ui_model_t *m)
{
    if (m == NULL) return;

    bool alarm = (m->state == RK_STATE_ALARM);
    set_visible(ui.alarm_root, alarm);
    set_visible(ui.p1_root, !alarm && m->page == RK_PAGE_GPU);
    set_visible(ui.p2_root, !alarm && m->page == RK_PAGE_SYS);

    if (alarm) {
        alarm_apply(m);
    } else {
        page1_apply(m);
        page2_apply(m);
    }
}

void rk_ui_refresh(const rk_ui_model_t *m)
{
    rk_ui_apply(m);
    /* 不整屏 invalidate：只有内容真变化的 widget 产生脏区；LVGL FULL 模式下
     * 无脏区则不 flush，配合 st7305 同帧跳推 = 静止画面零面板写入零闪屏 */
    lv_refr_now(lv_display_get_default());
}

void rk_ui_sleep_screen(void)
{
    set_visible(ui.p1_root, false);
    set_visible(ui.p2_root, false);
    set_visible(ui.alarm_root, false);
    set_visible(ui.sleep_root, true);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(lv_display_get_default());
}

rk_page_t rk_ui_page_next(rk_page_t p)
{
    return (p == RK_PAGE_GPU) ? RK_PAGE_SYS : RK_PAGE_GPU;
}

bool rk_alarm_hit(const rk_stats_t *s, bool *is_gpu, float *temp_c)
{
    if (s == NULL) return false;
    if (s->gpu.driver_present && s->gpu.driver && s->gpu.temp_c.present &&
        s->gpu.temp_c.value >= RK_TEMP_ALARM_C) {
        if (is_gpu != NULL) *is_gpu = true;
        if (temp_c != NULL) *temp_c = (float)s->gpu.temp_c.value;
        return true;
    }
    if (s->cpu.temp_c.present && s->cpu.temp_c.value >= RK_TEMP_ALARM_C) {
        if (is_gpu != NULL) *is_gpu = false;
        if (temp_c != NULL) *temp_c = (float)s->cpu.temp_c.value;
        return true;
    }
    return false;
}
