/*
 * rk_stats.c — rig-stats 快照解析实现（schema 方向）。
 * 词法走 rk_json（有界、零分配）；本文件只做 schema 语义。
 */
#include "rk_stats.h"

#include <stdio.h>
#include <string.h>

/* ---- 小工具：对象字段遍历骨架 ----
 * 每层：rkj_expect('{') → 循环读键 → dispatch → '}' → eof 由调用方定 */

static rkj_err_t read_num_or_null(rk_json_t *j, rk_num_t *out)
{
    bool is_int = false;
    int64_t iv = 0;
    double dv = 0.0;

    memset(out, 0, sizeof(*out));
    if (rkj_peek(j) == 'n') {
        return rkj_expect_null(j); /* JSON null → present=false */
    }
    rkj_err_t e = rkj_read_number(j, &is_int, &iv, &dv);
    if (e != RK_PARSE_OK) return e;
    out->present = true;
    out->value = is_int ? (double)iv : dv;
    return RK_PARSE_OK;
}

static rkj_err_t read_string_or_skip(rk_json_t *j, char *out, size_t cap)
{
    size_t n = 0;
    return rkj_read_string(j, out, cap, &n);
}

/* ---------- gpu 子对象 ---------- */

static rkj_err_t parse_gpu(rk_json_t *j, rk_gpu_t *g)
{
    rkj_err_t e = rkj_expect(j, '{');
    if (e != RK_PARSE_OK) return e;

    for (;;) {
        if (rkj_peek(j) == '}') break;
        char key[24];
        e = rkj_read_string(j, key, sizeof(key), NULL);
        if (e != RK_PARSE_OK) return e;
        e = rkj_expect(j, ':');
        if (e != RK_PARSE_OK) return e;

        if (strcmp(key, "name") == 0) {
            memset(g->name, 0, sizeof(g->name));
            if (rkj_peek(j) == 'n') {
                e = rkj_expect_null(j);
            } else {
                e = read_string_or_skip(j, g->name, sizeof(g->name));
            }
        }
        else if (strcmp(key, "temp_c") == 0) e = read_num_or_null(j, &g->temp_c);
        else if (strcmp(key, "util_pct") == 0) e = read_num_or_null(j, &g->util_pct);
        else if (strcmp(key, "vram_used_gb") == 0) e = read_num_or_null(j, &g->vram_used_gb);
        else if (strcmp(key, "vram_total_gb") == 0) e = read_num_or_null(j, &g->vram_total_gb);
        else if (strcmp(key, "power_w") == 0) e = read_num_or_null(j, &g->power_w);
        else if (strcmp(key, "power_limit_w") == 0) e = read_num_or_null(j, &g->power_limit_w);
        else if (strcmp(key, "fan_pct") == 0) e = read_num_or_null(j, &g->fan_pct);
        else if (strcmp(key, "driver") == 0) {
            if (rkj_peek(j) == 'n') {
                e = rkj_expect_null(j);
                g->driver_present = false;
            } else {
                e = rkj_read_bool(j, &g->driver);
                if (e == RK_PARSE_OK) g->driver_present = true;
            }
        }
        else e = rkj_skip_value(j); /* 未知键前向兼容 */
        if (e != RK_PARSE_OK) return e;
        if (rkj_peek(j) == ',') {
            e = rkj_expect(j, ',');
            if (e != RK_PARSE_OK) return e;
        }
    }
    return rkj_expect(j, '}');
}

/* ---------- cpu 子对象 ---------- */

static rkj_err_t parse_cpu(rk_json_t *j, rk_cpu_t *c)
{
    rkj_err_t e = rkj_expect(j, '{');
    if (e != RK_PARSE_OK) return e;

    for (;;) {
        if (rkj_peek(j) == '}') break;
        char key[24];
        e = rkj_read_string(j, key, sizeof(key), NULL);
        if (e != RK_PARSE_OK) return e;
        e = rkj_expect(j, ':');
        if (e != RK_PARSE_OK) return e;

        if (strcmp(key, "temp_c") == 0) e = read_num_or_null(j, &c->temp_c);
        else if (strcmp(key, "util_pct") == 0) e = read_num_or_null(j, &c->util_pct);
        else if (strcmp(key, "cores_pct") == 0) {
            if (rkj_peek(j) == 'n') {
                e = rkj_expect_null(j);
            } else {
                e = rkj_expect(j, '[');
                while (e == RK_PARSE_OK && rkj_peek(j) != ']') {
                    rk_num_t v;
                    e = read_num_or_null(j, &v);
                    if (e != RK_PARSE_OK) break;
                    if (c->core_count < RK_CORE_MAX) {
                        c->cores_present[c->core_count] = v.present;
                        c->cores_pct[c->core_count] = v.value;
                        c->core_count++;
                    }
                    if (rkj_peek(j) == ',') e = rkj_expect(j, ',');
                }
                if (e == RK_PARSE_OK) e = rkj_expect(j, ']');
            }
        }
        else e = rkj_skip_value(j);
        if (e != RK_PARSE_OK) return e;
        if (rkj_peek(j) == ',') {
            e = rkj_expect(j, ',');
            if (e != RK_PARSE_OK) return e;
        }
    }
    return rkj_expect(j, '}');
}

/* ---------- mem 子对象 ---------- */

static rkj_err_t parse_mem(rk_json_t *j, rk_mem_t *m)
{
    rkj_err_t e = rkj_expect(j, '{');
    if (e != RK_PARSE_OK) return e;

    for (;;) {
        if (rkj_peek(j) == '}') break;
        char key[24];
        e = rkj_read_string(j, key, sizeof(key), NULL);
        if (e != RK_PARSE_OK) return e;
        e = rkj_expect(j, ':');
        if (e != RK_PARSE_OK) return e;

        if (strcmp(key, "used_gb") == 0) e = read_num_or_null(j, &m->used_gb);
        else if (strcmp(key, "total_gb") == 0) e = read_num_or_null(j, &m->total_gb);
        else if (strcmp(key, "util_pct") == 0) e = read_num_or_null(j, &m->util_pct);
        else if (strcmp(key, "swap_used_gb") == 0) e = read_num_or_null(j, &m->swap_used_gb);
        else if (strcmp(key, "swap_total_gb") == 0) e = read_num_or_null(j, &m->swap_total_gb);
        else e = rkj_skip_value(j);
        if (e != RK_PARSE_OK) return e;
        if (rkj_peek(j) == ',') {
            e = rkj_expect(j, ',');
            if (e != RK_PARSE_OK) return e;
        }
    }
    return rkj_expect(j, '}');
}

/* ---------- sys 子对象（含 disks 数组） ---------- */

static rkj_err_t parse_disk(rk_json_t *j, rk_disk_t *d)
{
    rkj_err_t e = rkj_expect(j, '{');
    if (e != RK_PARSE_OK) return e;
    memset(d, 0, sizeof(*d));

    for (;;) {
        if (rkj_peek(j) == '}') break;
        char key[24];
        e = rkj_read_string(j, key, sizeof(key), NULL);
        if (e != RK_PARSE_OK) return e;
        e = rkj_expect(j, ':');
        if (e != RK_PARSE_OK) return e;

        if (strcmp(key, "mnt") == 0) e = read_string_or_skip(j, d->mnt, sizeof(d->mnt));
        else if (strcmp(key, "free_gb") == 0) e = read_num_or_null(j, &d->free_gb);
        else if (strcmp(key, "total_gb") == 0) e = read_num_or_null(j, &d->total_gb);
        else e = rkj_skip_value(j);
        if (e != RK_PARSE_OK) return e;
        if (rkj_peek(j) == ',') {
            e = rkj_expect(j, ',');
            if (e != RK_PARSE_OK) return e;
        }
    }
    return rkj_expect(j, '}');
}

static rkj_err_t parse_sys(rk_json_t *j, rk_sys_t *s)
{
    rkj_err_t e = rkj_expect(j, '{');
    if (e != RK_PARSE_OK) return e;

    for (;;) {
        if (rkj_peek(j) == '}') break;
        char key[24];
        e = rkj_read_string(j, key, sizeof(key), NULL);
        if (e != RK_PARSE_OK) return e;
        e = rkj_expect(j, ':');
        if (e != RK_PARSE_OK) return e;

        if (strcmp(key, "load1") == 0) e = read_num_or_null(j, &s->load1);
        else if (strcmp(key, "net_rx_kbps") == 0) e = read_num_or_null(j, &s->net_rx_kbps);
        else if (strcmp(key, "net_tx_kbps") == 0) e = read_num_or_null(j, &s->net_tx_kbps);
        else if (strcmp(key, "disks") == 0) {
            if (rkj_peek(j) == 'n') {
                e = rkj_expect_null(j);
            } else {
                e = rkj_expect(j, '[');
                while (e == RK_PARSE_OK && rkj_peek(j) != ']') {
                    rk_disk_t d;
                    e = parse_disk(j, &d);
                    if (e != RK_PARSE_OK) break;
                    if (s->disk_count < RK_DISK_MAX) {
                        s->disks[s->disk_count++] = d;
                    }
                    if (rkj_peek(j) == ',') e = rkj_expect(j, ',');
                }
                if (e == RK_PARSE_OK) e = rkj_expect(j, ']');
            }
        }
        else e = rkj_skip_value(j);
        if (e != RK_PARSE_OK) return e;
        if (rkj_peek(j) == ',') {
            e = rkj_expect(j, ',');
            if (e != RK_PARSE_OK) return e;
        }
    }
    return rkj_expect(j, '}');
}

/* ---------- 顶层 ---------- */

rk_parse_result_t rk_stats_parse(const uint8_t *bytes, size_t len, rk_stats_t *out)
{
    rk_json_t j;
    rkj_err_t e;

    if (bytes == NULL || out == NULL || len == 0 || len > RK_STATS_JSON_MAX_BYTES) {
        return RK_PARSE_ERR_SIZE;
    }
    memset(out, 0, sizeof(*out));

    rkj_init(&j, bytes, len);
    if (rkj_expect(&j, '{') != RK_PARSE_OK) return RK_PARSE_ERR_FIELD;

    for (;;) {
        if (rkj_peek(&j) == '}') break;
        char key[24];
        e = rkj_read_string(&j, key, sizeof(key), NULL);
        if (e != RK_PARSE_OK) return e;
        e = rkj_expect(&j, ':');
        if (e != RK_PARSE_OK) return e;

        if (strcmp(key, "schema") == 0) {
            bool is_int = false;
            int64_t v = 0;
            e = rkj_read_number(&j, &is_int, &v, NULL);
            if (e == RK_PARSE_OK) out->schema = is_int ? (int)v : 0;
        }
        else if (strcmp(key, "host") == 0) e = read_string_or_skip(&j, out->host, sizeof(out->host));
        else if (strcmp(key, "ts") == 0) {
            bool is_int = false;
            int64_t v = 0;
            if (rkj_peek(&j) == 'n') {
                e = rkj_expect_null(&j);
            } else {
                e = rkj_read_number(&j, &is_int, &v, NULL);
                if (e == RK_PARSE_OK) {
                    out->ts = v;
                    out->ts_present = is_int;
                }
            }
        }
        else if (strcmp(key, "net_if") == 0) e = read_string_or_skip(&j, out->net_if, sizeof(out->net_if));
        else if (strcmp(key, "uptime_s") == 0) {
            bool is_int = false;
            int64_t v = 0;
            if (rkj_peek(&j) == 'n') {
                e = rkj_expect_null(&j);
            } else {
                e = rkj_read_number(&j, &is_int, &v, NULL);
                if (e == RK_PARSE_OK) {
                    out->uptime_s = v;
                    out->uptime_present = is_int;
                }
            }
        }
        else if (strcmp(key, "gpu") == 0) e = parse_gpu(&j, &out->gpu);
        else if (strcmp(key, "cpu") == 0) e = parse_cpu(&j, &out->cpu);
        else if (strcmp(key, "mem") == 0) e = parse_mem(&j, &out->mem);
        else if (strcmp(key, "sys") == 0) e = parse_sys(&j, &out->sys);
        else e = rkj_skip_value(&j);
        if (e != RK_PARSE_OK) return e;
        if (rkj_peek(&j) == ',') {
            e = rkj_expect(&j, ',');
            if (e != RK_PARSE_OK) return e;
        }
    }
    e = rkj_expect(&j, '}');
    if (e != RK_PARSE_OK) return e;
    return rkj_expect_eof(&j);
}

bool rk_netif_is_wifi(const char *net_if)
{
    return net_if != NULL && net_if[0] == 'w' && net_if[1] == 'l';
}

void rk_uptime_text(int64_t uptime_s, char *out, size_t cap)
{
    if (uptime_s <= 0) {
        snprintf(out, cap, "--");
        return;
    }
    int64_t d = uptime_s / 86400;
    int64_t h = (uptime_s % 86400) / 3600;
    int64_t m = (uptime_s % 3600) / 60;
    if (d > 0) snprintf(out, cap, "%lldD%lldH", (long long)d, (long long)h);
    else if (h > 0) snprintf(out, cap, "%lldH%lldM", (long long)h, (long long)m);
    else snprintf(out, cap, "%lldM", (long long)m);
}
