/*
 * main.c — rig-lookout 主程序（P5 睡眠机制版）。
 *
 * 目标板：Waveshare ESP32-S3-RLCD-4.2（独板，1301 人格烧录）。
 * 事件驱动主循环：按键 ISR / poll 数据 / 10s 兜底三类事件唤醒，醒→处理→渲染
 * →电源服务；配合 tickless idle，事件间隙 CPU 真睡（电池在位时；无电池持
 * NO_LIGHT_SLEEP 锁保 USB 控制台）。
 *
 * 电源服务（P5 定稿 2026-09-17）：
 *   - 轻睡门控：电池不在位（<2500mV）禁轻睡；在位放行（console 排障取舍）
 *   - WiFi 降档：CALM 持续 5min → MAX_MODEM；BUSY/警报/按键恢复 MIN_MODEM
 *   - 低压深睡：电池在位且 <3.65V 连续 3 次（负载毛刺防抖）→ 清屏只显
 *     「休眠中」→ 停射频 → 深睡 1h 复查 + BOOT 即时唤醒（≥3.75V 迟滞放行）
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "lvgl_port.h"
#include "rk_stats.h"
#include "rk_ui.h"
#include "rig_batt.h"
#include "rig_env.h"
#include "rig_ev.h"
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

#define BTN_BOOT_GPIO 0
#define BTN_KEY_GPIO 18
#define BTN_DEBOUNCE_MS 30

/* P5 电源/睡眠参数（2026-09-17 用户定稿） */
#define BATT_USB_MV          2500   /* < 此值 = 无电池（USB 供电，信使同款推断） */
#define BATT_SLEEP_MV        3650   /* 低于此值 → 深睡（用户定稿 3.65V） */
#define BATT_WAKE_OK_MV      3750   /* 深睡唤醒复查通过阈值（迟滞带） */
#define BATT_SLEEP_CONFIRM   3      /* 连续 N 次低于阈值才睡（防负载毛刺） */
/* 低压深睡复查周期（2026-09-18 用户修订：要「插 USB 即有响应」的体验）。
 * 物理约束：插充电器不复位深睡芯片（电池维持 3V3）、无 USB 在位唤醒引脚，
 * 即时插电复活不可达。折中=每 1h 一次纯电压复查（~3-5s，无 WiFi/无上报，
 * ~1.7mAh/天 ≈ 2500mAh 电芯 0.07%/天，实测可忽略）；插电后电压秒抬 ~4.1V
 * → 下一整点复查必过 → 复活。急性子路径=BOOT 键（ext0）立即唤醒复查。
 * 手动断电=长按 PWR。 */
#define DEEP_SLEEP_RECHECK_S 3600
#define PS_MAX_AFTER_CALM_MS 300000 /* CALM 持续 5min → WiFi 降 MAX_MODEM */
#define BATT_POLL_MS         10000  /* 电池采样节律 */
#define EV_TIMEOUT_MS        10000  /* 主循环事件等待兜底（同电池节律） */
#define BTN_WINDOW_N         15     /* 按键事件后 150ms 消抖采样窗 */
#define BTN_WINDOW_STEP_MS   10

static rk_poll_state_t g_poll;
static rk_ui_model_t g_model;
static rk_page_t g_page = RK_PAGE_GPU;

/* ---- 走势：服务端真源 + 双缓冲（poll 任务写 / UI 主任务读，临界区交换） ---- */
static float g_trend_a[RK_HIST_VIEW_MAX], g_trend_b[RK_HIST_VIEW_MAX];
static float *g_trend_cur = g_trend_a;
static int g_trend_len;

/* ---- 电池 / 电源服务状态 ---- */
static char g_batt_text[10];
static int64_t g_batt_last_ms;
static int32_t g_batt_mv = -1;   /* 最近一次有效原始读数（mV）；-1=尚无有效读数 */
static int g_batt_low_count;     /* 连续低于深睡阈值计数 */
static bool g_sleep_now;
static bool g_ps_max_active;     /* 当前是否 MAX_MODEM */
static int64_t g_calm_since_ms = -1;

/* ---- 事件信号（按键 ISR / poll 完成） ---- */
static SemaphoreHandle_t g_ev;

/* 深睡原因 RTC 标记（跨深睡存活；下次开机以 wake_lowbatt 事件补报后清除） */
RTC_DATA_ATTR static uint32_t g_rtc_lowbatt;

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t g_no_ls; /* 电池不在位：禁轻睡保 USB 控制台 */
#endif

/* ---- 环境温湿度（板载 SHTC3，60s 低频采样；失败保留旧值） ---- */
#define ENV_POLL_MS 60000u
static char g_env_text[16];
static int64_t g_env_last_ms;

static void env_poll(void)
{
    int64_t now = (int64_t)(esp_timer_get_time() / 1000LL);
    if (g_env_last_ms != 0 && now - g_env_last_ms < ENV_POLL_MS) {
        return;
    }
    g_env_last_ms = (now != 0) ? now : 1;
    float t, h;
    if (rig_env_read(&t, &h) == ESP_OK) {
        snprintf(g_env_text, sizeof g_env_text, "%.1fC %.0f%%", t, h);
    }
}

/* ---- 电池采样（节律 + 文本 + 原始 mV） ---- */
static void batt_poll(void)
{
    int64_t now = (int64_t)(esp_timer_get_time() / 1000LL);
    if (g_batt_last_ms != 0 && now - g_batt_last_ms < BATT_POLL_MS) {
        return;
    }
    g_batt_last_ms = (now != 0) ? now : 1;
    int32_t mv = rig_batt_read_mv();
    if (mv >= 0) {
        g_batt_mv = mv;
    }
    if (mv < 0) {
        snprintf(g_batt_text, sizeof g_batt_text, "--");
    } else if (mv < BATT_USB_MV) {
        snprintf(g_batt_text, sizeof g_batt_text, "USB");
    } else {
        snprintf(g_batt_text, sizeof g_batt_text, "%.2fV", mv / 1000.0);
    }
}

/* ---- 按键：事件唤醒 + 采样窗消抖（首调采样不触发，防幻影翻页） ---- */
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

static void btn_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(g_ev, &woken);
    portYIELD_FROM_ISR(woken);
}

#if RK_HAS_NET_CONFIG
/* poll 任务：快照 + 节拍 FSM（rig_poll）+ 曲线回补（rig_hist），完成后唤醒主循环 */
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
        xSemaphoreGive(g_ev); /* 新数据 → 唤醒主循环渲染 */
        vTaskDelay(pdMS_TO_TICKS(delay_ms)); /* 哨兵节拍：BUSY 2s / CALM 10s */
    }
}
#endif

/* poll 状态 → UI 模型（临界区内拷贝快照与走势指针） */
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
    g_model.env_text = g_env_text;
    g_model.gpu_busy = g_model.stats.gpu.util_pct.present &&
                       g_model.stats.gpu.util_pct.value >= 15.0; /* 标题行「渲染中」牌 */
}

static void app_render(void)
{
    rk_ui_refresh(&g_model);
}

/* ---- 电源/睡眠服务（每轮事件后） ---- */
static void power_service(void)
{
    batt_poll();
    env_poll();

#if CONFIG_PM_ENABLE
    /* 轻睡门控：电池不在位持锁保 USB 控制台；在位放行真睡 */
    static bool ls_released;
    if (g_batt_mv >= 0 && g_batt_mv < BATT_USB_MV) {
        if (ls_released) {
            esp_pm_lock_acquire(g_no_ls);
            ls_released = false;
            rig_ev("ls", "gated");
            ESP_LOGI(TAG, "light-sleep gated (no battery, usb console kept)");
        }
    } else if (g_batt_mv >= BATT_USB_MV) {
        if (!ls_released) {
            esp_pm_lock_release(g_no_ls);
            ls_released = true;
            rig_ev("ls", "armed");
            ESP_LOGW(TAG, "light-sleep ARMED (battery present)");
        }
    }
#endif

    /* 低压深睡：电池在位且连续 N 次低于 3.65V（无电池/读数无效不判） */
    if (g_batt_mv >= BATT_USB_MV) {
        if (g_batt_mv < BATT_SLEEP_MV) {
            g_batt_low_count++;
            if (g_batt_low_count == 1) {
                rig_ev("lowbatt", "%dmV", (int)g_batt_mv); /* 首次越线即记（确认过程看 board.jsonl b 值） */
            }
            if (g_batt_low_count >= BATT_SLEEP_CONFIRM) {
                g_sleep_now = true;
            }
        } else {
            g_batt_low_count = 0;
        }
    } else {
        g_batt_low_count = 0;
    }

#if RK_HAS_NET_CONFIG
    /* WiFi 射频降档：CALM 持续 5min → MAX_MODEM；BUSY/警报/按键恢复 MIN_MODEM */
    bool calm = (g_model.state != RK_STATE_ALARM) &&
                (g_poll.cadence == RK_CAD_CALM);
    int64_t now = (int64_t)(esp_timer_get_time() / 1000LL);
    if (calm) {
        if (g_calm_since_ms < 0) {
            g_calm_since_ms = now;
        }
        if (!g_ps_max_active && now - g_calm_since_ms >= PS_MAX_AFTER_CALM_MS) {
            rig_wifi_set_ps(true);
            rig_ev("ps", "max");
            g_ps_max_active = true;
        }
    } else {
        g_calm_since_ms = -1;
        if (g_ps_max_active) {
            rig_wifi_set_ps(false);
            rig_ev("ps", "min");
            g_ps_max_active = false;
        }
    }

    /* 信标 + 控制台心跳（每 60 个事件 ≈10min 一行） */
    rk_poll_beacon_set(g_batt_mv, calm);
    static int s_hb;
    if (++s_hb >= 60) {
        s_hb = 0;
        ESP_LOGI(TAG, "hb: state=%d cad=%s batt=%dmV trend=%d",
                 (int)g_model.state, calm ? "calm" : "busy", g_batt_mv, g_trend_len);
    }
#endif
}

/* ---- 低压深睡（终端屏 + 1h 定时 / BOOT 唤醒复查） ---- */
static void deep_sleep_now(void)
{
    g_rtc_lowbatt = 1; /* RTC 标记跨深睡（下次开机补报 wake_lowbatt） */
    rig_ev("deep_sleep", "b=%dmV", (int)g_batt_mv); /* 本条大概率来不及上传，靠 RTC 补报 */
    ESP_LOGW(TAG, "battery %dmV < %dmV x%d -> deep sleep", g_batt_mv, BATT_SLEEP_MV,
             g_batt_low_count);
#if RK_HAS_NET_CONFIG
    esp_wifi_stop(); /* 停射频；面板 RAM 常供电保持「休眠中」 */
#endif
    rk_ui_sleep_screen();
    /* BOOT(ext0, GPIO0 RTC pad 低电平) 可立即唤醒复查；另每 1h 纯电压复查 */
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep(DEEP_SLEEP_RECHECK_S); /* 不返回 */
}

/* 开机电池复查：电芯在位但仍低于唤醒阈值 → 休眠屏 + 回睡（不连 WiFi） */
static bool boot_battery_gate(void)
{
    int32_t mv = rig_batt_read_mv();
    if (mv < 0) {
        return true; /* ADC 异常：可用性优先，放行（低压防线仍由运行期监测兜底） */
    }
    if (mv >= BATT_USB_MV && mv < BATT_WAKE_OK_MV) {
        ESP_LOGW(TAG, "boot: battery %dmV still low -> back to sleep", mv);
        return false;
    }
    return true;
}

void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "rig-lookout P5 sleep build (esp32s3, LVGL 9.3.0, %s, pm=%d)",
             RK_HAS_NET_CONFIG ? "net" : "offline-demo",
#if CONFIG_PM_ENABLE
             1
#else
             0
#endif
    );
    rig_ev("boot", "rst=%d fw=P5s", (int)esp_reset_reason());
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP && g_rtc_lowbatt) {
        rig_ev("wake_lowbatt", "rtc=1");
        g_rtc_lowbatt = 0;
    }

    /* 按键：输入 + 上拉 + 双沿中断（事件驱动唤醒） */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_BOOT_GPIO) | (1ULL << BTN_KEY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io);

    g_ev = xSemaphoreCreateBinary();

    /* 电池 ADC + 温湿度（失败不挡启动：对应位显 "--"/空） */
    if (rig_batt_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery adc unavailable");
    }
    if (rig_env_init() != ESP_OK) {
        ESP_LOGW(TAG, "env sensor unavailable");
    }

#if CONFIG_PM_ENABLE
    {
        esp_pm_config_t pm = {
            .max_freq_mhz = 240,
            .min_freq_mhz = 40,
        };
        esp_pm_configure(&pm);
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "rk_nols", &g_no_ls);
        esp_pm_lock_acquire(g_no_ls); /* 默认禁轻睡；电池在位后由 power_service 放行 */
        ESP_LOGI(TAG, "pm ready: tickless+dfs, light-sleep gated until battery present");
    }
#endif

    /* 显示链路（低压门要用屏） */
    st7305_config_t panel_cfg = st7305_default_config();
    err = st7305_init(&panel_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "st7305_init failed: %s", esp_err_to_name(err));
        return;
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

    /* 开机低压门：仍低 → 休眠屏 + 回睡（1h 后或 BOOT 再查） */
    if (!boot_battery_gate()) {
        rk_ui_sleep_screen();
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); /* BOOT 唤醒复查 */
        esp_deep_sleep(DEEP_SLEEP_RECHECK_S); /* 仍低 → 回睡（1h 后再查） */
    }

#if RK_HAS_NET_CONFIG
    /* WiFi STA 先行（异步连接，事件驱动重连） */
    ESP_ERROR_CHECK(rig_wifi_start(RK_WIFI_SSID, RK_WIFI_PASS));
#endif

    model_from_poll();
    app_render();

    /* 按键 ISR 注册 */
    if (gpio_install_isr_service(0) != ESP_OK) {
        ESP_LOGW(TAG, "isr service install failed (button falls back to window sampling)");
    } else {
        gpio_isr_handler_add(BTN_BOOT_GPIO, btn_isr, NULL);
        gpio_isr_handler_add(BTN_KEY_GPIO, btn_isr, NULL);
    }

#if RK_HAS_NET_CONFIG
    xTaskCreate(poll_task, "rig_poll", 8192, NULL, 5, &g_poll_task);
#endif

    /* 事件驱动主循环：醒 → 按键窗 → 模型/渲染 → 电源服务 →（低压则深睡） */
    for (;;) {
        lv_timer_handler();

        if (xSemaphoreTake(g_ev, pdMS_TO_TICKS(EV_TIMEOUT_MS)) == pdTRUE) {
            /* 按键事件后 150ms 消抖采样窗（其他事件源误触发无害） */
            for (int i = 0; i < BTN_WINDOW_N; i++) {
                if (any_button_short_press()) {
                    g_page = rk_ui_page_next(g_page);
                    g_calm_since_ms = -1; /* 按键=活动：power_service 会恢复 MIN_MODEM */
                    rig_ev("page", "%d", (int)g_page);
                    ESP_LOGI(TAG, "button short press -> page %d", (int)g_page);
                }
                vTaskDelay(pdMS_TO_TICKS(BTN_WINDOW_STEP_MS));
            }
        }

        model_from_poll();
        app_render();
        power_service();
        if (g_sleep_now) {
            deep_sleep_now();
        }
    }
}
