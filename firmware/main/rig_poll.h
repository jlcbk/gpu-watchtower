/*
 * rig_poll.h — rig-stats HTTP 轮询骨架（P2a；真机联调属 P3）。
 *
 * 职责：周期 GET http://<host>:<port>/stats（X-Token 头），2s 节拍（PLAN §5）；
 * 连续失败 ≥3 次 → OFFLINE（指数退避重连，不轰炸）；成功快照经 rk_stats_parse
 * 进 rk_ui_model_t。警报态（≥85°C 持续 30s，迟滞防抖）在固件侧合成。
 *
 * 凭据纪律（沿用源项目红线）：host/port/token 只经 rig_net_config.h 注入
 * （gitignore，绝不入库）；无该文件时 main.c #error，离线演示构建用
 * RK_DEVNET_OFFLINE=1（不编入任何凭据，UI 如实显 OFFLINE 态）。
 */
#ifndef RIG_POLL_H
#define RIG_POLL_H

#include <stdbool.h>

#include "rk_stats.h"
#include "rk_ui.h"

/* 哨兵节拍（P5 电源卡 v2）：双档轮询节奏 */
typedef enum {
    RK_CAD_CALM = 0, /* 慢档 10s：常态（GPU 双态分布，实时性要求弱） */
    RK_CAD_BUSY = 1  /* 快档 2s：活跃（util≥15% / 温度≥75°C / 警报带） */
} rk_cadence_t;

/* 轮询输出：最近快照 + 固件侧合成状态 */
typedef struct {
    rk_stats_t stats;
    bool have_snapshot;
    rk_dev_state_t state;     /* NORMAL / OFFLINE / NODRIVER / ALARM */
    rk_cadence_t cadence;     /* 当前节拍档（决定 next_delay_ms 基准） */
    float alarm_temp_c;
    bool alarm_is_gpu;
    char last_online_hhmm[6]; /* 最后一次成功快照时刻（ts+时区，"HH:MM"） */
    uint32_t next_delay_ms;   /* 本次 step 后建议的下次轮询间隔（busy 2s / calm 10s / 失败退避） */
} rk_poll_state_t;

/* 单次轮询：成功 = RK_PARSE_OK 且 out 内快照更新；失败（超时/403/解析错）计连击。
 * 由 poll 任务周期调用；状态机内部维护连击/退避/警报迟滞。 */
void rk_poll_step(rk_poll_state_t *st);

/* 板子信标（随每次 /stats 轮询上报，rig-stats 服务端落 JSONL 日志）：
 * b=电池 mV（-1=无读数）、c=节拍档（1=calm/0=busy）、r=WiFi RSSI dBm。 */
void rk_poll_beacon_set(int32_t batt_mv, bool calm);

/* 警报迟滞参数（P4 实测后调；测试便利：允许固件侧覆盖） */
#ifndef RK_ALARM_SUSTAIN_MS
#define RK_ALARM_SUSTAIN_MS 30000u
#endif
#ifndef RK_ALARM_CLEAR_C
#define RK_ALARM_CLEAR_C 83.0
#endif

#endif /* RIG_POLL_H */
