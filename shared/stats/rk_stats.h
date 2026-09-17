/*
 * rk_stats.h — rig-stats 快照 schema（PLAN §4）与解析。
 *
 * 协议真源：rig-lookout/PLAN.md §4（GET /stats 快照 JSON）。
 * 铁律：**任何字段可 null**，解析后对应 rk_num_t.present=false，显示层显 "--"
 * 不崩（PLAN §4 schema 注释；cdt_json 浮点空指针教训的正面清单）。
 * 本模块零动态分配；输入预算 RK_STATS_JSON_MAX_BYTES。
 */
#ifndef RK_STATS_H
#define RK_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rk_json.h"

/* 快照整包字节预算在 rk_json.h（RK_STATS_JSON_MAX_BYTES，词法层输入上限） */
/* 单字符串字段容量（含 NUL） */
#define RK_STR_CAP 48
/* 盘条目上限（当前协议 2 条：/ 与 /mnt/model-library） */
#define RK_DISK_MAX 4
/* CPU 核数上限（主机 6 核，PLAN §3） */
#define RK_CORE_MAX 8

/* 可空标量：present=false 即 JSON null / 缺失 */
typedef struct {
    bool present;
    double value;
} rk_num_t;

typedef struct {
    char mnt[RK_STR_CAP];
    rk_num_t free_gb;
    rk_num_t total_gb;
} rk_disk_t;

typedef struct {
    char name[RK_STR_CAP];
    rk_num_t temp_c;
    rk_num_t util_pct;
    rk_num_t vram_used_gb;
    rk_num_t vram_total_gb;
    rk_num_t power_w;
    rk_num_t power_limit_w;
    rk_num_t fan_pct;
    bool driver_present; /* driver 字段是否出现 */
    bool driver;         /* false → 驱动未装态（PLAN §5） */
} rk_gpu_t;

typedef struct {
    rk_num_t temp_c;
    rk_num_t util_pct;
    double cores_pct[RK_CORE_MAX];
    bool cores_present[RK_CORE_MAX];
    int core_count; /* cores_pct 数组实际长度（clamp 到 RK_CORE_MAX） */
} rk_cpu_t;

typedef struct {
    rk_num_t used_gb;
    rk_num_t total_gb;
    rk_num_t util_pct;
    rk_num_t swap_used_gb;
    rk_num_t swap_total_gb;
} rk_mem_t;

typedef struct {
    rk_num_t load1;
    rk_num_t net_rx_kbps;
    rk_num_t net_tx_kbps;
    rk_disk_t disks[RK_DISK_MAX];
    int disk_count;
} rk_sys_t;

typedef struct {
    int schema;
    char host[RK_STR_CAP];
    bool ts_present;
    int64_t ts; /* UTC 秒 */
    char net_if[RK_STR_CAP];
    bool uptime_present;
    int64_t uptime_s;
    rk_gpu_t gpu;
    rk_cpu_t cpu;
    rk_mem_t mem;
    rk_sys_t sys;
} rk_stats_t;

/* 解析整个快照。任何语法/类型错误 → 非 OK 且 *out 不承诺内容。
 * 未知键跳过（前向兼容）；null → present=false。 */
rk_parse_result_t rk_stats_parse(const uint8_t *bytes, size_t len, rk_stats_t *out);

/* net_if 是否以 "wl" 开头（WiFi 链路标记，PLAN §5；wlx… 是 USB 网卡命名） */
bool rk_netif_is_wifi(const char *net_if);

/* uptime_s → "3D12H" / "5H42M" / "18M"（>0 段进位截断，缺项 "--"） */
void rk_uptime_text(int64_t uptime_s, char *out, size_t cap);

/* 阈值（PLAN §5 初值；P4 实测后调） */
#define RK_TEMP_TICK_C 75.0 /* 走势线刻度 */
#define RK_TEMP_HEAVY_C 80.0 /* 温度区加重（反白小块） */
#define RK_TEMP_ALARM_C 85.0 /* 超温警报（持续 30s 迟滞在固件侧） */

#endif /* RK_STATS_H */
