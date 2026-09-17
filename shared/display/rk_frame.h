/*
 * rk_frame.h — 共享单色逻辑帧（P1.4，A0）
 *
 * 契约：PLAN rig-lookout §5 共享逻辑帧格式——400 宽×300 高、1bit/像素、
 * 行优先、每行 50 字节、字节内 MSB 对应左像素、1=黑/0=白，共 15000 字节。
 * 该格式用于测试/SDL 输出；ST7305 适配器另行转换（P4.2）。
 * 越界坐标一律忽略（防御性，不崩溃）。
 */
#ifndef RK_FRAME_H
#define RK_FRAME_H

#include <stdint.h>

#define RK_FRAME_WIDTH 400
#define RK_FRAME_HEIGHT 300
#define RK_FRAME_STRIDE 50 /* (WIDTH+7)/8 */
#define RK_FRAME_BYTES (RK_FRAME_STRIDE * RK_FRAME_HEIGHT) /* 15000 */

typedef struct {
    uint8_t px[RK_FRAME_BYTES];
} rk_frame_t;

/* value：非 0 = 黑（置 1），0 = 白（清 0）。*/
void rk_frame_clear(rk_frame_t *f, int black);
void rk_frame_set(rk_frame_t *f, int x, int y, int black);
int rk_frame_get(const rk_frame_t *f, int x, int y); /* 返回 0/1；越界返回 -1 */

#endif /* RK_FRAME_H */
