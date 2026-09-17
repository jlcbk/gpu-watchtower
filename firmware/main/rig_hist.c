/*
 * rig_hist.c — /history 拉取 + 解析 + 降采样（哨兵节拍）。
 *
 * 服务端格式（exporter/server.py）：{"schema":1,"step_s":10,"latest_ts":T,
 * "points":[[ts,temp,util],...]}（点仅含 temp 非 null 项，oldest→newest）。
 * 客户端取尾部 360 点（60min @10s/点）按 stride 3 降采样为 ≤120 点，
 * 与 UI「TREND 60MIN」标签口径一致。
 */
#include "rig_hist.h"

#include <stdlib.h>
#include <string.h>

#include <esp_log.h>

#if !defined(RK_DEVNET_OFFLINE)
#include "rig_net_config.h"
#include "esp_http_client.h"
#endif

#define TAG "rig_hist"

#define HIST_FETCH_LIMIT  380 /* ~63min @10s/点：覆盖 60min 窗口 + 裕量 */
#define HIST_WINDOW_POINTS 360
#define HIST_STRIDE        3

#if !defined(RK_DEVNET_OFFLINE)
EXT_RAM_BSS_ATTR static char s_body[12288];
static float s_pts[HIST_FETCH_LIMIT];
#endif

esp_err_t rig_hist_fetch_view(int64_t after_ts, float *view, int *view_len,
                              int64_t *latest_ts)
{
#if defined(RK_DEVNET_OFFLINE)
    (void)after_ts; (void)view;
    if (view_len != NULL) *view_len = 0;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (view == NULL || view_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *view_len = 0;

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/history?after=%lld&limit=%d",
             RK_STATS_HOST, RK_STATS_PORT, (long long)after_ts, HIST_FETCH_LIMIT);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 2500,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) return ESP_FAIL;
    esp_http_client_set_header(cli, "X-Token", RK_STATS_TOKEN);
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(cli);
        return err;
    }
    esp_http_client_fetch_headers(cli);
    int len = esp_http_client_read_response(cli, s_body, sizeof(s_body) - 1);
    if (len < 0) len = 0;
    s_body[len] = '\0';
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (status != 200 || len <= 0) {
        ESP_LOGW(TAG, "history HTTP %d (%d bytes)", status, len);
        return ESP_FAIL;
    }
    if (latest_ts != NULL) {
        const char *lt = strstr(s_body, "\"latest_ts\":");
        if (lt != NULL) {
            *latest_ts = strtoll(lt + 12, NULL, 10);
        }
    }

    /* 解析 points 数组（服务端格式固定；util 字段忽略，temp 恒为数字） */
    const char *p = strstr(s_body, "\"points\"");
    if (p == NULL) return ESP_ERR_INVALID_RESPONSE;
    p = strchr(p + 8, '[');
    if (p == NULL) return ESP_ERR_INVALID_RESPONSE;
    p++; /* 进外层数组 */
    int n = 0;
    while (*p != '\0' && n < HIST_FETCH_LIMIT) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;
        if (*p != '[') return ESP_ERR_INVALID_RESPONSE;
        p++;
        char *end;
        (void)strtoll(p, &end, 10); /* ts */
        p = end;
        while (*p == ' ' || *p == ',') p++;
        s_pts[n++] = strtof(p, &end); /* temp */
        p = end;
        while (*p != '\0' && *p != ']') p++; /* 跳过 util */
        if (*p == ']') p++;
    }

    /* 尾部 60min 窗口 stride 降采样 → 视图 */
    int win = (n > HIST_WINDOW_POINTS) ? HIST_WINDOW_POINTS : n;
    int m = 0;
    for (int i = n - win; i < n; i += HIST_STRIDE) {
        view[m++] = s_pts[i];
    }
    if (m >= RK_HIST_VIEW_MAX) m = RK_HIST_VIEW_MAX;
    *view_len = m;
    ESP_LOGI(TAG, "hist ok: %d pts -> view %d", n, m);
    return ESP_OK;
#endif /* RK_DEVNET_OFFLINE */
}
