/*
 * rig_ev.h — 板内事件环形缓冲（暂存 + 上传 rig-stats 落 events.jsonl）。
 *
 * 事件 = {"t":<开机毫秒>,"e":"<名>","v":"<值>"}；drop-oldest 环形 48 槽（PSRAM）。
 * poll 任务在成功轮询后 POST /beacon 批量上传，成功才提交（失败下轮重试）。
 */
#ifndef RIG_EV_H
#define RIG_EV_H

#include <stdint.h>

/* 记一条事件；fmt==NULL 则无 v 字段；超长截断到槽宽。中断上下文禁止调用。 */
void rig_ev(const char *name, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* 取待上传批量（NDJSON 文本，内部缓冲指针）；*count_out=包含条数；空返回 NULL。 */
const char *rig_ev_drain(int *len_out, int *count_out);

/* 上传成功后提交（丢弃已上传条数，条数 = drain 返回的 count）。 */
void rig_ev_commit(int count);

#endif /* RIG_EV_H */
