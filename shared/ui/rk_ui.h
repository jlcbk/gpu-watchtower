/*
 * rk_ui.h — rig-lookout 共享 UI（固件 / 模拟器双端同一份，LVGL 9.3.0）。
 *
 * 双页 × 四态（PLAN §5）：
 *   页 1 GPU   ：超大温度数字一瞥层 + UTIL/VRAM/PWR/FAN + 底部 60min 温度走势
 *                （75°C 虚线刻度）；net_if 为 wl* 时标题区显 WiFi 标记。
 *   页 2 SYS   ：CPU 总占用+温度 + 6 核每核小条 + RAM/SWAP 条 + 盘余量 +
 *                网速/load/uptime。
 *   四态       ：NORMAL / OFFLINE（主机离线·最后在线 hh:mm 横幅 + 数值全 "--"）/
 *                NODRIVER（gpu.driver=false → GPU 区显「驱动未装」，SYS 照常）/
 *                ALARM（GPU 或 CPU ≥85°C 固件侧触发 → 整屏反白警报牌，抢占两页）。
 *
 * 布局契约：400x300、纯黑白、外边距 8、标题行 y=8 高 24、底栏 y=270 高 22
 * （沿用源项目设计语言）。UI 层禁 IO：数据由宿主经 rk_ui_model_t 注入。
 */
#ifndef RK_UI_H
#define RK_UI_H

#include <stdbool.h>
#include <stddef.h>

#include "rk_frame.h"
#include "rk_stats.h"
#include "lvgl.h"

/* 设备侧状态（固件侧合成：离线=轮询连续超时；警报=温度阈值迟滞；均不由 JSON 表达） */
typedef enum {
    RK_STATE_NORMAL = 0,
    RK_STATE_OFFLINE = 1,
    RK_STATE_NODRIVER = 2,
    RK_STATE_ALARM = 3
} rk_dev_state_t;

typedef enum {
    RK_PAGE_GPU = 0,
    RK_PAGE_SYS = 1
} rk_page_t;

/* UI 模型：宿主每拍填充后调 rk_ui_apply。零动态分配（trend 数组归宿主持有）。 */
typedef struct {
    rk_dev_state_t state;
    rk_page_t page;
    rk_stats_t stats;         /* 最近一次合法快照（离线时可为最后快照或全缺） */
    bool have_snapshot;       /* 是否收到过合法快照 */
    const char *last_online;  /* OFFLINE 横幅："21:04"（宿主格式化）；NULL → "--" */
    const char *clock_text;   /* 底栏右 "HH:MM"（宿主格式化）；NULL → "--" */
    const char *batt_text;    /* 底栏电池位："4.05V" / "USB"（宿主格式化）；NULL/"" → 隐藏 */
    const char *env_text;     /* 底栏环境位："24.9C 36%"（板载 SHTC3）；NULL/"" → 隐藏 */
    bool gpu_busy;            /* 标题行「渲染中」反白牌（util≥15%）；空载不显示 */
    /* 60min 走势（°C，trend[0] 最旧 → trend[len-1] 最新；宿主持有存储） */
    const float *trend;
    int trend_len;
    /* ALARM 牌内容 */
    float alarm_temp_c;       /* 触发温度 */
    bool alarm_is_gpu;        /* true=GPU false=CPU */
} rk_ui_model_t;

/* lv_init 后调用一次：建三根容器（page1/page2/alarm）并显初始页。 */
void rk_ui_init(void);

/* 模型 → 全部 widget；宿主随后 lv_refr_now（或用 rk_ui_refresh）。 */
void rk_ui_apply(const rk_ui_model_t *m);

/* apply + 整屏失效 + lv_refr_now（同步整帧刷新，语义同源项目 port）。 */
void rk_ui_refresh(const rk_ui_model_t *m);

/* 终端屏（P5 低压深睡）：清全部页面只显居中「休眠中」并同步刷新（不返回常态）。 */
void rk_ui_sleep_screen(void);

/* BOOT 短按翻页（0↔1）小工具；纯函数。 */
rk_page_t rk_ui_page_next(rk_page_t p);

/* 温度阈值判定（固件侧警报 FSM 用；纯函数便于双端单测）。
 * 返回该快照是否处于 ≥RK_TEMP_ALARM_C（GPU 优先，其次 CPU），hit 温度写出。 */
bool rk_alarm_hit(const rk_stats_t *s, bool *is_gpu, float *temp_c);

#endif /* RK_UI_H */
