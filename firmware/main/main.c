/*
 * main.c — rig-lookout 固件骨架（P2a）。
 *
 * 目标板：Waveshare ESP32-S3-RLCD-4.2（第三块板，独立项目；PLAN §2）。
 * 启动序列：ST7305 面板 init（推白帧）→ LVGL port（400x300 I1 FULL）→
 * rk_ui_init（双页 × 四态）→ ui 任务（10ms lv_timer_handler + BOOT 翻页 +
 * 1Hz 重渲染）。轮询任务（rig_poll）骨架就位：有 rig_net_config.h 时 2s
 * 周期 GET /stats 并驱动四态；离线演示构建（RK_DEVNET_OFFLINE=1）不编入
 * 任何凭据，UI 如实显「主机离线」。
 *
 * 交互：BOOT 短按翻页（页 1 GPU ↔ 页 2 SYS，纯手动常驻无自动回跳，PLAN §5）；
 * 警报随时抢占（rig_poll 状态机迟滞），恢复回原页。
 *
 * 烧录/真机联调不在本卡范围（P3）；本任务只保证 esp32s3 目标构建产出 .bin。
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "lvgl_port.h"
#include "rk_stats.h"
#include "rk_ui.h"
#include "rig_batt.h"
#include "rig_hist.h"
#include "rig_poll.h"
#include "rig_wifi.h"
#include "st7305.h"

/* ---- 凭据守卫（沿用源项目红线：默认凭据绝不编译） ---- */
#if defined(RK_DEVNET_OFFLINE)
#define RK_HAS_NET_CONFIG 0
#elif defined(__has_include)
#if __has_include("rig_net_config.h")
#include "rig_net_config.h"
#define RK_HAS_NET_CONFIG 1
#else
#error "缺少 firmware/main/rig_net_config.h：cp firmware/main/rig_net_config.h.template " \
       "firmware/main/rig_net_config.h 并按模板注释填入（该文件已 gitignore）。" \
       "无网演示构建：idf.py -C firmware build -DRK_DEVNET_OFFLINE=1"
#endif
#else
#include "rig_net_config.h"
#define RK_HAS_NET_CONFIG 1
#endif

#define TAG "rk_app"

#define APP_TASK_STACK_BYTES (12 * 1024)
#define APP_TICK_MS 10
#define RENDER_PERIOD_MS 1000 /* 1Hz 重渲染（无脏区时 LVGL 不 flush，不闪屏） */
#define BTN_BOOT_GPIO 0       /* BOOT 键：短按翻页（PLAN §5 交互） */
#define BTN_KEY_GPIO 18       /* KEY 键（GPIO18 低有效，Waveshare 官方表）：翻页别名 */
#define BTN_DEBOUNCE_MS 30

/* ---- 共享模型（ui 任务独占写；poll 结果经互斥交接简化为同一任务执行 step） ----
 * P2a 骨架：poll step 与 UI 同任务串行（省互斥）；P3 真机联调再拆任务+队列。 */
static rk_poll_state_t g_poll;
static rk_ui_model_t g_model;
static rk_page_t g_page = RK_PAGE_GPU;

/* ---- 走势：服务端真源 + 双缓冲（哨兵节拍：曲线记忆在 rig-stats /history，
 *      板端零状态；poll 任务写 / UI 主任务读，经 g_poll_mux 临界区交换指针） ---- */
static float g_trend_a[RK_HIST_VIEW_MAX], g_trend_b[RK_HIST_VIEW_MAX];
static float *g_trend_cur = g_trend_a;
static int g_trend_len;

/* ---- 电池：10s 节律采样 → "4.05V" / "USB"（<2.5V 无电池，信使同款判定） ---- */
#define BATT_POLL_MS 10000u
static char g_batt_text[10];
static int64_t g_batt_last_ms;

static void batt_poll(void)
{
    int64_t now = (int64_t)(esp_timer_get_time() / 1000LL);
    if (g_batt_last_ms != 0 && now - g_batt_last_ms < BATT_POLL_MS) {
        return;
    }
    g_batt_last_ms = (now != 0) ? now : 1;
    int32_t mv = rig_batt_read_mv();
    if (mv < 0) {
        snprintf(g_batt_text, sizeof g_batt_text, "--");
    } else if (mv < 2500) {
        snprintf(g_batt_text, sizeof g_batt_text, "USB");
    } else {
        snprintf(g_batt_text, sizeof g_batt_text, "%.2fV", mv / 1000.0);
    }
}

#if RK_HAS_NET_CONFIG
/* 正常构建：poll step 阻塞 HTTP（2s 超时）会拖慢 UI 节拍 → 独立低优先级任务，
 * 结果经临界区交接（结构体小，关中断拷贝足够）。 */
static portMUX_TYPE g_poll_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t g_poll_task;
static void poll_task(void *arg)
{
    static rk_poll_state_t local;
    static int64_t hist_last_ts; /* 上次已取到的服务端最新点 ts（下次 after 基准） */
    static int64_t t_last_hist;
    for (;;) {
        rk_poll_step(&local);

        /* 曲线回补：开机首拉 + 此后每 30s（服务端 12h@10s 历史 → 120 点视图） */
        int64_t now = (int64_t)(esp_timer_get_time() / 1000LL);
        if (local.have_snapshot &&
            (t_last_hist == 0 || now - t_last_hist >= 30000)) {
            t_last_hist = now;
            float *nxt = (g_trend_cur == g_trend_a) ? g_trend_b : g_trend_a;
            int vn = 0;
            int64_t lt = 0;
            int64_t after = (hist_last_ts > 3630) ? hist_last_ts - 3630 : 0;
            if (rig_hist_fetch_view(after, nxt, &vn, &lt) == ESP_OK &&
                vn > 1 && lt > 0) {
                hist_last_ts = lt;
                taskENTER_CRITICAL(&g_poll_mux);
                g_trend_cur = nxt;
                g_trend_len = vn;
                taskEXIT_CRITICAL(&g_poll_mux);
            }
        }

        uint32_t delay_ms = local.next_delay_ms ? local.next_delay_ms : 2000u;
        taskENTER_CRITICAL(&g_poll_mux);
        g_poll = local;
        taskEXIT_CRITICAL(&g_poll_mux);
        vTaskDelay(pdMS_TO_TICKS(delay_ms)); /* 哨兵节拍：BUSY 2s / CALM 10s（退避在 rig_poll 内） */
    }
}
#endif

/* 两键等价翻页（BOOT=GPIO0 / KEY=GPIO18，均低有效内部上拉）。
 * 消抖为「电平变化计时 + 稳定 BTN_DEBOUNCE_MS 后确认」；首调只采样不触发
 * （旧版 stable_high 初值与 pressed 不一致，开机第一循环会幻影翻页一次——
 * 2026-09-17 真机「卡在系统页」的根因）。释放沿计一次短按。 */
typedef struct {
    bool last, stable, inited;
    int64_t t_change;
} btn_state_t;
static btn_state_t g_btn[2];

static bool btn_short_press(int gpio, btn_state_t *st)
{
    bool now = gpio_get_level(gpio) == 0;
    int64_t t = (int64_t)(esp_timer_get_time() / 1000LL);

    if (!st->inited) {
        st->inited = true;
        st->last = now;
        st->stable = now;
        st->t_change = t;
        return false;
    }
    if (now != st->last) {
        st->last = now;
        st->t_change = t;
    } else if ((t - st->t_change) >= BTN_DEBOUNCE_MS && st->last != st->stable) {
        st->stable = st->last;
        if (!st->stable) return true; /* 释放沿 = 一次短按 */
    }
    return false;
}

static bool any_button_short_press(void)
{
    return btn_short_press(BTN_BOOT_GPIO, &g_btn[0]) ||
           btn_short_press(BTN_KEY_GPIO, &g_btn[1]);
}

/* poll 状态 → UI 模型（同任务/临界区内调用） */
static void model_from_poll(void)
{
#if RK_HAS_NET_CONFIG
    taskENTER_CRITICAL(&g_poll_mux);
#endif
    g_model.stats = g_poll.stats;
    g_model.have_snapshot = g_poll.have_snapshot;
    g_model.state = g_poll.state;
    g_model.trend = (g_trend_len > 1) ? g_trend_cur : NULL; /* 1 点画不了线，藏 */
    g_model.trend_len = g_trend_len;
#if RK_HAS_NET_CONFIG
    taskEXIT_CRITICAL(&g_poll_mux);
#endif
    g_model.page = g_page;
    g_model.alarm_temp_c = g_poll.alarm_temp_c;
    g_model.alarm_is_gpu = g_poll.alarm_is_gpu;
    g_model.last_online = g_poll.last_online_hhmm;
    g_model.clock_text = g_poll.have_snapshot ? g_poll.last_online_hhmm : "--";
    g_model.batt_text = g_batt_text;
    g_model.gpu_busy = g_model.stats.gpu.util_pct.present &&
                       g_model.stats.gpu.util_pct.value >= 15.0; /* 标题行「渲染中」牌 */
    batt_poll();
}

static void app_render(void)
{
    rk_ui_refresh(&g_model);
}

void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "rig-lookout P2a skeleton (esp32s3, LVGL 9.3.0, %s build)",
             RK_HAS_NET_CONFIG ? "net" : "offline-demo");

    /* BOOT + KEY 按键：输入 + 上拉（两键等价翻页） */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_BOOT_GPIO) | (1ULL << BTN_KEY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    /* 电池 ADC（失败不挡启动：标题行显 "--"） */
    if (rig_batt_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery adc unavailable");
    }

#if RK_HAS_NET_CONFIG
    /* WiFi STA 先行（异步连接，事件驱动重连）：面板初始化期间已在握手；
     * 未取到 IP 前 poll 不计连击，UI 诚实停在初始离线态 */
    ESP_ERROR_CHECK(rig_wifi_start(RK_WIFI_SSID, RK_WIFI_PASS));
#endif

    /* 显示链路：面板 → LVGL port → UI */
    st7305_config_t panel_cfg = st7305_default_config();
    err = st7305_init(&panel_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "st7305_init failed: %s", esp_err_to_name(err));
        return; /* 无屏不出帧（P3 真机再定降级策略） */
    }
    err = rk_lvgl_port_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl port init failed: %s", esp_err_to_name(err));
        return;
    }

    memset(&g_poll, 0, sizeof(g_poll));
    memset(&g_model, 0, sizeof(g_model));
    g_poll.state = RK_STATE_OFFLINE; /* 首帧诚实显离线；首个合法快照后转正常 */
    g_model.last_online = "--";
    g_model.clock_text = "--";
    rk_ui_init();
    model_from_poll();
    app_render();

#if RK_HAS_NET_CONFIG
    xTaskCreate(poll_task, "rig_poll", 8192, NULL, 5, &g_poll_task);
#endif

    /* 主循环：10ms 节拍（lv_timer_handler；时基 = LV_TICK_CUSTOM esp_timer） */
    int64_t t_render = 0;
    for (;;) {
        lv_timer_handler();

        if (any_button_short_press()) {
            g_page = rk_ui_page_next(g_page);
            ESP_LOGI(TAG, "button short press -> page %d", (int)g_page);
            model_from_poll();
            app_render();
        }

        model_from_poll();

        int64_t now = (int64_t)(esp_timer_get_time() / 1000LL);
        if (now - t_render >= RENDER_PERIOD_MS) {
            t_render = now;
            app_render();
        }
        vTaskDelay(pdMS_TO_TICKS(APP_TICK_MS));
    }
}
