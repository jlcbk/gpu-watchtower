/*
 * rig_wifi.h — WiFi STA 拉起（P3 真机首刷）。
 *
 * 适配自 codex-desk-terminal dev_net.c 的精简版：事件驱动自动重连，
 * 无 PS 档联动/idle 降档（本屏常供电常轮询，用不上）。
 */
#ifndef RIG_WIFI_H
#define RIG_WIFI_H

#include <stdbool.h>

#include "esp_err.h"

/* 启动 STA 并发起异步连接（立刻返回；断开事件内自动重连） */
esp_err_t rig_wifi_start(const char *ssid, const char *pass);

/* 是否已取到 IP（poll 侧据此决定是否计连击/退避） */
bool rig_wifi_connected(void);

#endif /* RIG_WIFI_H */
