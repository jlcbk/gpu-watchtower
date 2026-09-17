/*
 * rig_wifi.c — WiFi STA 接线（P3 真机；适配自 codex-desk-terminal dev_net.c）。
 *
 * 家网同名 SSID 多 AP 环境对策（2026-09-17 实测：2.4G 上存在第二个广播 "Cui"
 * 的路由器，子网 192.168.188.x，误连后够不着 192.168.1.x 服务器）：
 *   - 启动先扫同名 AP（按 RSSI 降序），锁定最强 BSSID 连接（防漫游）；
 *   - GOT_IP 子网不符 → 轮换到下一候选 BSSID 重连（循环取模，好 AP 回线即自愈）。
 * NVS→netif→event loop→STA；断线事件回调内重连（非阻塞，IDF 例程模式）。
 */
#include "rig_wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#define TAG "rig_wifi"

/* 目标局域网段（须与 rig_net_config.h 的 RK_STATS_HOST 同段） */
#define RK_LAN_A 192
#define RK_LAN_B 168
#define RK_LAN_C 1

#define MAX_CAND 4

static volatile bool s_connected;
static volatile bool s_allow_connect;
static wifi_ap_record_t s_cand[MAX_CAND];
static int s_cand_count;
static int s_cand_idx;
static int s_wrong_logged; /* 无好 AP 时的循环日志限流 */

static void log_cand(int i)
{
    ESP_LOGI(TAG, "cand[%d/%d] bssid %02x:%02x:%02x:%02x:%02x:%02x ch%u rssi %d",
             i + 1, s_cand_count,
             s_cand[i].bssid[0], s_cand[i].bssid[1], s_cand[i].bssid[2],
             s_cand[i].bssid[3], s_cand[i].bssid[4], s_cand[i].bssid[5],
             (unsigned)s_cand[i].primary, (int)s_cand[i].rssi);
}

/* 轮换到下一候选并重连（先改 config 再 disconnect，让断线回调用新 BSSID） */
static void rotate_and_disconnect(void)
{
    if (s_cand_count > 1) {
        s_cand_idx = (s_cand_idx + 1) % s_cand_count;
        wifi_config_t wc;
        if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
            memcpy(wc.sta.bssid, s_cand[s_cand_idx].bssid, 6);
            wc.sta.bssid_set = true;
            esp_wifi_set_config(WIFI_IF_STA, &wc);
        }
        if ((++s_wrong_logged % 8) == 1) { /* 全坏时循环打日志限流 */
            log_cand(s_cand_idx);
        }
    }
    esp_wifi_disconnect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_allow_connect) {
            ESP_LOGI(TAG, "STA start -> connect");
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) {
            ESP_LOGW(TAG, "wifi disconnected -> auto reconnect");
        }
        s_connected = false;
        if (s_allow_connect) {
            esp_wifi_connect(); /* 用当前 config（可能已被轮换）重连 */
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        if (esp_ip4_addr1(&ev->ip_info.ip) == RK_LAN_A &&
            esp_ip4_addr2(&ev->ip_info.ip) == RK_LAN_B &&
            esp_ip4_addr3(&ev->ip_info.ip) == RK_LAN_C) {
            s_connected = true;
        } else {
            ESP_LOGW(TAG, "wrong subnet -> rotate AP and retry");
            rotate_and_disconnect();
        }
    }
}

esp_err_t rig_wifi_start(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    strncpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid - 1);
    strncpy((char *)wc.sta.password, pass ? pass : "", sizeof wc.sta.password - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; /* 开放/WEP 拒绝 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_LOGI(TAG, "connecting ssid=\"%s\"（密码不落日志）", ssid);
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 同名 AP 扫描（结果按 RSSI 降序）：锁定最强 BSSID，防漫游到异子网路由器 */
    wifi_scan_config_t sc = {
        .ssid = (const uint8_t *)ssid,
    };
    esp_wifi_scan_start(&sc, true);
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 0) {
        wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
        if (recs != NULL) {
            esp_wifi_scan_get_ap_records(&n, recs);
            for (uint16_t i = 0; i < n && s_cand_count < MAX_CAND; i++) {
                if (strcmp((const char *)recs[i].ssid, ssid) == 0) {
                    s_cand[s_cand_count++] = recs[i];
                }
            }
            free(recs);
        }
    }
    for (int i = 0; i < s_cand_count; i++) {
        log_cand(i);
    }
    if (s_cand_count == 0) {
        ESP_LOGW(TAG, "scan found no \"%s\" AP, connect blind", ssid);
    } else {
        wifi_config_t wc2;
        if (esp_wifi_get_config(WIFI_IF_STA, &wc2) == ESP_OK) {
            memcpy(wc2.sta.bssid, s_cand[0].bssid, 6);
            wc2.sta.bssid_set = true;
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc2));
        }
    }

    s_allow_connect = true;
    esp_wifi_connect();
    return ESP_OK;
}

bool rig_wifi_connected(void)
{
    return s_connected;
}
