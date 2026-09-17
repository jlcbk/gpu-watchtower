/*
 * rig_ev.c — 事件环形缓冲实现（哨兵节拍姊妹件：记忆外包给服务器）。
 */
#include "rig_ev.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

#define EV_SLOTS    48u
#define EV_SLOT_LEN 112u

EXT_RAM_BSS_ATTR static char s_ev[EV_SLOTS][EV_SLOT_LEN];
static uint16_t s_head;   /* 下一个写入槽 */
static uint16_t s_used;   /* 环内总条数（含未上传） */
static uint16_t s_unsent; /* 待上传条数 */
EXT_RAM_BSS_ATTR static char s_send[EV_SLOTS * EV_SLOT_LEN]; /* 上传批量拼装缓冲 */

void rig_ev(const char *name, const char *fmt, ...)
{
    if (name == NULL) {
        return;
    }
    int n;
    if (fmt != NULL) {
        char val[64];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(val, sizeof val, fmt, ap);
        va_end(ap);
        n = snprintf(s_ev[s_head], EV_SLOT_LEN,
                     "{\"t\":%lld,\"e\":\"%s\",\"v\":\"%s\"}",
                     (long long)(esp_timer_get_time() / 1000LL), name, val);
    } else {
        n = snprintf(s_ev[s_head], EV_SLOT_LEN, "{\"t\":%lld,\"e\":\"%s\"}",
                     (long long)(esp_timer_get_time() / 1000LL), name);
    }
    (void)n;
    s_head = (uint16_t)((s_head + 1) % EV_SLOTS);
    if (s_used < EV_SLOTS) {
        s_used++;
    } /* 满则覆盖最旧（drop-oldest） */
    if (s_unsent < EV_SLOTS) {
        s_unsent++;
    }
}

const char *rig_ev_drain(int *len_out, int *count_out)
{
    if (len_out != NULL) *len_out = 0;
    if (count_out != NULL) *count_out = 0;
    if (s_unsent == 0) {
        return NULL;
    }
    int off = 0;
    int included = 0;
    uint16_t idx = (uint16_t)((s_head + EV_SLOTS - s_used) % EV_SLOTS); /* 最旧 */
    for (uint16_t i = 0; i < s_used; i++) {
        int l = (int)strnlen(s_ev[idx], EV_SLOT_LEN);
        if (off + l + 1 >= (int)sizeof s_send) {
            break; /* 缓冲满：剩余留待下轮 */
        }
        memcpy(s_send + off, s_ev[idx], l);
        off += l;
        s_send[off++] = '\n';
        included++;
        idx = (uint16_t)((idx + 1) % EV_SLOTS);
    }
    if (off == 0) {
        return NULL;
    }
    if (len_out != NULL) *len_out = off;
    if (count_out != NULL) *count_out = included;
    return s_send;
}

void rig_ev_commit(int count)
{
    if (count <= 0) {
        return;
    }
    if ((uint16_t)count > s_used) {
        count = s_used;
    }
    s_used = (uint16_t)(s_used - count);  /* 已上传条目出环 */
    s_unsent = s_used;                    /* 环内剩余全部视为已对齐（下轮新事件再报） */
}
