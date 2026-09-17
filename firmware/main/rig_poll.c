/*
 * rig_poll.c — rig-stats HTTP 轮询（P2a 骨架 + P3 真机首刷修）。
 *
 * 离线演示构建（RK_DEVNET_OFFLINE=1）：本文件退化为「永远离线」桩——
 * 不编入 esp_http_client/凭据，UI 如实显 OFFLINE。
 * 正常构建：WiFi 已连（rig_wifi_connected）才发起 GET（open → fetch_headers →
 * read_response），成功 2s 节拍、失败指数退避（经 st->next_delay_ms 交接给
 * poll 任务）；WiFi 未连上不计连击不退避（首次快照前停在初始 OFFLINE）。
 */
#include "rig_poll.h"

#include <stdio.h>
#include <string.h>

#include <esp_log.h>

#include "esp_timer.h"

#if !defined(RK_DEVNET_OFFLINE)
#include "rig_net_config.h" /* gitignore 凭据注入（main.c #error 守卫已保证存在） */
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "rig_ev.h"
#include "rig_wifi.h"
#endif

#define TAG "rig_poll"

/* 状态机参数（PLAN §5 + 哨兵节拍） */
#define OFFLINE_AFTER_FAILS 3u
#define POLL_PERIOD_MS 2000u   /* BUSY 档周期 */
#define CALM_PERIOD_MS 10000u  /* CALM 档周期（P5 v2：双态分布，实时性弱） */
#define CAD_CALM_STREAK 5u     /* 降档迟滞：连续 N 个平静周期才回 CALM */

#if !defined(RK_DEVNET_OFFLINE)

typedef struct {
    uint32_t fail_streak;
    uint32_t calm_streak;     /* BUSY 期间连续平静计数（降档迟滞） */
    int64_t last_ok_ms;       /* 最后成功时刻（esp_timer ms） */
    int64_t alarm_since_ms;   /* ≥85°C 起始时刻（<0 = 未进入） */
    bool alarm_latched;
    uint32_t backoff_ms;      /* 指数退避（离线重连不轰炸） */
} rk_poll_fsm_t;

static rk_poll_fsm_t g_fsm = {
    .fail_streak = 0,
    .calm_streak = 0,
    .last_ok_ms = -1,
    .alarm_since_ms = -1,
    .alarm_latched = false,
    .backoff_ms = POLL_PERIOD_MS,
};

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000LL);
}

/* ---- 板子信标（服务端日志用） ---- */
static int32_t s_beacon_mv = -1;
static int s_beacon_calm = 1;
static int s_beacon_rssi = 0;

void rk_poll_beacon_set(int32_t batt_mv, bool calm)
{
    s_beacon_mv = batt_mv;
    s_beacon_calm = calm ? 1 : 0;
}

static void hhmm_from_ts(int64_t ts, char *out, size_t cap)
{
    /* Kconfig 时区偏移（默认 +480 = 中国）；仅显示换算 */
    int64_t t = ts + (int64_t)CONFIG_RK_TZ_OFFSET_MIN * 60LL;
    snprintf(out, cap, "%02lld:%02lld", (long long)((t / 3600LL) % 24LL),
             (long long)((t / 60LL) % 60LL));
}

/* 警报迟滞：进入需 ≥85°C 持续 30s；解除需 <83°C（迟滞带防抖） */
static void alarm_update(rk_poll_state_t *st)
{
    bool is_gpu = false;
    float t = 0.0f;
    bool hit = rk_alarm_hit(&st->stats, &is_gpu, &t);
    int64_t now = now_ms();

    if (st->state == RK_STATE_OFFLINE) {
        g_fsm.alarm_since_ms = -1; /* 离线无数据，警报挂起（恢复后重新计 30s） */
        return;
    }
    if (hit) {
        if (g_fsm.alarm_since_ms < 0) g_fsm.alarm_since_ms = now;
        if (!g_fsm.alarm_latched &&
            (uint64_t)(now - g_fsm.alarm_since_ms) >= RK_ALARM_SUSTAIN_MS) {
            g_fsm.alarm_latched = true;
            st->state = RK_STATE_ALARM;
            st->alarm_temp_c = t;
            st->alarm_is_gpu = is_gpu;
            ESP_LOGW(TAG, "ALARM latched: %s %.1fC (>=85C %ums)",
                     is_gpu ? "GPU" : "CPU", (double)t,
                     (unsigned)RK_ALARM_SUSTAIN_MS);
        }
    } else {
        double cur = st->stats.gpu.temp_c.present ? st->stats.gpu.temp_c.value : 0.0;
        double cur_cpu = st->stats.cpu.temp_c.present ? st->stats.cpu.temp_c.value : 0.0;
        if (cur < RK_ALARM_CLEAR_C && cur_cpu < RK_ALARM_CLEAR_C) {
            g_fsm.alarm_since_ms = -1;
            if (g_fsm.alarm_latched) {
                g_fsm.alarm_latched = false;
                ESP_LOGI(TAG, "ALARM cleared (<%.0fC hysteresis)", (double)RK_ALARM_CLEAR_C);
            }
        }
    }
}

/* 事件批量上传：成功轮询后顺手 POST /beacon；200 才提交（失败下轮重试） */
static esp_err_t post_events(void)
{
    int len = 0, count = 0;
    const char *body = rig_ev_drain(&len, &count);
    if (body == NULL) {
        return ESP_OK;
    }

    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d/beacon", RK_STATS_HOST, RK_STATS_PORT);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 2500,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_method(cli, HTTP_METHOD_POST);
    esp_http_client_set_header(cli, "X-Token", RK_STATS_TOKEN);
    esp_http_client_set_header(cli, "Content-Type", "application/x-ndjson");
    esp_err_t err = esp_http_client_open(cli, len);
    if (err == ESP_OK) {
        int w = esp_http_client_write(cli, body, len);
        esp_http_client_fetch_headers(cli);
        char rsp[64];
        (void)esp_http_client_read_response(cli, rsp, sizeof rsp - 1);
        int status = esp_http_client_get_status_code(cli);
        if (w == len && status == 200) {
            rig_ev_commit(count);
            err = ESP_OK;
        } else {
            ESP_LOGW(TAG, "beacon POST w=%d/%d status=%d", w, len, status);
            err = ESP_FAIL;
        }
    }
    esp_http_client_cleanup(cli);
    return err;
}

/* 单次 GET /stats：open → fetch_headers → read_response（正文可靠进 body）。
 * 注意 esp_http_client_set_header 的第三参是纯 value——传 "X-Token: xxx" 整串
 * 会变成双重头名导致永远 403。 */
static esp_err_t poll_once(rk_stats_t *out, bool *have)
{
    char url[128];
    esp_err_t err;
    int status = 0;
    static char body[RK_STATS_JSON_MAX_BYTES + 1];

    /* 信标参数随轮询上报（b=电池mV c=节拍 r=RSSI）；RSSI 顺路取一次 */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_beacon_rssi = ap.rssi;
    }
    snprintf(url, sizeof(url), "http://%s:%d/stats?b=%ld&c=%d&r=%d",
             RK_STATS_HOST, RK_STATS_PORT, (long)s_beacon_mv, s_beacon_calm,
             (int)s_beacon_rssi);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 2000, /* 2s 节拍内的阻塞预算 */
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) return ESP_FAIL;

    esp_http_client_set_header(cli, "X-Token", RK_STATS_TOKEN); /* token 不落日志 */
    *have = false;
    err = esp_http_client_open(cli, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        int len = esp_http_client_read_response(cli, body, (int)sizeof(body) - 1);
        if (len < 0) len = 0;
        body[len] = '\0';
        status = esp_http_client_get_status_code(cli);
        if (status == 200 && len > 0) {
            if (rk_stats_parse((const uint8_t *)body, (size_t)len, out) == RK_PARSE_OK) {
                *have = true;
            } else {
                ESP_LOGW(TAG, "stats parse failed (%d bytes)", len);
                err = ESP_ERR_INVALID_RESPONSE;
            }
        } else if (status == 403) {
            ESP_LOGW(TAG, "403: token mismatch (host %s)", RK_STATS_HOST);
            err = ESP_ERR_INVALID_STATE;
        } else {
            ESP_LOGW(TAG, "HTTP %d (%d bytes) from %s", status, len, RK_STATS_HOST);
            err = ESP_FAIL;
        }
    }
    esp_http_client_cleanup(cli);
    return err;
}

void rk_poll_step(rk_poll_state_t *st)
{
    rk_stats_t snap;
    bool have = false;
    esp_err_t err;

    if (!rig_wifi_connected()) {
        /* WiFi 未就绪：不计连击、退避归零（连上后 ≤2s 出首帧数据） */
        g_fsm.backoff_ms = POLL_PERIOD_MS;
        st->next_delay_ms = POLL_PERIOD_MS;
        return;
    }

    err = poll_once(&snap, &have);

    if (err == ESP_OK && have) {
        bool was_offline = (st->state == RK_STATE_OFFLINE);
        g_fsm.fail_streak = 0;
        g_fsm.backoff_ms = POLL_PERIOD_MS;
        g_fsm.last_ok_ms = now_ms();
        st->stats = snap;
        st->have_snapshot = true;
        hhmm_from_ts(snap.ts, st->last_online_hhmm, sizeof st->last_online_hhmm);
        st->next_delay_ms = POLL_PERIOD_MS;

        /* 状态仲裁：ALARM > NODRIVER(gpu.driver=false) > NORMAL（OFFLINE 由失败侧判定） */
        if (snap.gpu.driver_present && !snap.gpu.driver) {
            st->state = RK_STATE_NODRIVER;
        } else {
            st->state = RK_STATE_NORMAL;
        }
        alarm_update(st); /* 可升为 ALARM（迟滞 30s） */
        if (was_offline) {
            rig_ev("online", "gpu=%.0fC",
                   snap.gpu.temp_c.present ? snap.gpu.temp_c.value : 0.0);
            ESP_LOGI(TAG, "back online (gpu %s%.0fC)",
                     snap.gpu.temp_c.present ? "" : "--",
                     snap.gpu.temp_c.present ? snap.gpu.temp_c.value : 0.0);
        }

        /* 哨兵节拍：升级=util≥15% / 温度≥75°C（兜住渲片间隙 util 假跌）/ 警报带；
         * 降级=BUSY 期间连续 CAD_CALM_STREAK 个平静周期（迟滞防抖） */
        bool trig = (snap.gpu.util_pct.present && snap.gpu.util_pct.value >= 15.0) ||
                    (snap.gpu.temp_c.present && snap.gpu.temp_c.value >= 75.0) ||
                    (st->state == RK_STATE_ALARM);
        if (trig) {
            g_fsm.calm_streak = 0;
            if (st->cadence != RK_CAD_BUSY) {
                st->cadence = RK_CAD_BUSY;
                rig_ev("cad", "busy");
                ESP_LOGI(TAG, "cadence -> BUSY (2s)");
            }
        } else if (st->cadence == RK_CAD_BUSY) {
            g_fsm.calm_streak++;
            if (g_fsm.calm_streak >= CAD_CALM_STREAK) {
                st->cadence = RK_CAD_CALM;
                g_fsm.calm_streak = 0;
                rig_ev("cad", "calm");
                ESP_LOGI(TAG, "cadence -> CALM (10s)");
            }
        }
        st->next_delay_ms = (st->cadence == RK_CAD_BUSY) ? POLL_PERIOD_MS : CALM_PERIOD_MS;
        post_events(); /* 事件批量上传（失败保留待重试） */
    } else {
        g_fsm.fail_streak++;
        if (g_fsm.fail_streak >= OFFLINE_AFTER_FAILS) {
            if (st->state != RK_STATE_OFFLINE) {
                rig_ev("offline", "%ufails", (unsigned)g_fsm.fail_streak);
                ESP_LOGW(TAG, "offline after %u consecutive failures",
                         (unsigned)g_fsm.fail_streak);
            }
            st->state = RK_STATE_OFFLINE; /* 警报让位离线；恢复后重计迟滞 */
        }
        /* 指数退避重连（封顶 30s，不轰炸） */
        g_fsm.backoff_ms *= 2;
        if (g_fsm.backoff_ms > 30000u) g_fsm.backoff_ms = 30000u;
        st->next_delay_ms = g_fsm.backoff_ms;
    }
}

#else /* RK_DEVNET_OFFLINE：离线演示构建，无网络凭据，永远 OFFLINE（不伪造在线） */

void rk_poll_step(rk_poll_state_t *st)
{
    st->state = RK_STATE_OFFLINE; /* 骨架：真机走正常构建分支 */
    st->next_delay_ms = POLL_PERIOD_MS;
}

#endif /* RK_DEVNET_OFFLINE */
