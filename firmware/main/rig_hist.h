/*
 * rig_hist.h — 服务端温度历史拉取（哨兵节拍卡：曲线唯一真源 = rig-stats /history）。
 */
#ifndef RK_HIST_H
#define RK_HIST_H

#include <stdint.h>

#include "esp_err.h"

#define RK_HIST_VIEW_MAX 120 /* 60min 窗口 @30s 等效粒度（UI 逐点绘制上限同值） */

/* 拉取 after_ts 之后的温度历史，降采样为 ≤120 点视图（oldest→newest）写入 view。
 * 成功时 view_len>0 且 *latest_ts=服务端最新点时间戳（作下次 after 基准）。
 * 失败时视图不动（沿用旧曲线），调用方按旧节奏重试。 */
esp_err_t rig_hist_fetch_view(int64_t after_ts, float *view, int *view_len,
                              int64_t *latest_ts);

#endif /* RK_HIST_H */
