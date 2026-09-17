/*
 * rk_json.h — 有界 JSON 词法原语。
 *
 * rig-lookout 副本（P2a）：从 codex-desk-terminal shared/state/cdt_json.c/.h
 * 拷贝改造（符号改名 cdtj_→rkj_；错误码枚举随文件本地化，不再依赖上游
 * codex_state.h）。词法行为与上游逐字节一致（同一份实现）。
 *
 * 设计约束（PLAN §4：任何字段可 null，缺项显 "--" 不崩）：
 *   - 零动态分配：调用方提供一切缓冲。
 *   - 输入即预算：整包 <=RK_STATS_JSON_MAX_BYTES（调用方在进入本层前检查）。
 *   - 嵌套深度 <=RK_JSON_MAX_DEPTH（根对象计 1），越界 ERR_DEPTH。
 *   - 字符串按 UTF-8 字节预算校验（cap），并验证 UTF-8 合法性（含 \u 代理对）。
 *   - 本层不了解 stats schema 语义；schema 方向解析见 rk_stats.c。
 */
#ifndef RK_JSON_H
#define RK_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 解析结果码（本地化枚举，取值语义沿用上游 cdt_parse_result_t 编号） */
typedef enum {
    RK_PARSE_OK = 0,
    RK_PARSE_ERR_VERSION = 2,
    RK_PARSE_ERR_SIZE = 3,
    RK_PARSE_ERR_DEPTH = 4,
    RK_PARSE_ERR_FIELD = 5,
    RK_PARSE_ERR_TRUNCATED_CP = 6,
    RK_PARSE_ERR_UNKNOWN_ENUM = 7
} rk_parse_result_t;

/* JSON 嵌套深度上限（stats 快照最深层 = 根.gpu/…/sys.disks[] = 4 层，取 8 留余） */
#define RK_JSON_MAX_DEPTH 8

/* 快照整包字节预算（词法层输入上限；观测样例 <1KB，4KB 余量充足） */
#define RK_STATS_JSON_MAX_BYTES 4096

typedef rk_parse_result_t rkj_err_t;

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    int depth; /* 当前已进入的容器层数（根为 1） */
} rk_json_t;

static inline void rkj_init(rk_json_t *j, const void *bytes, size_t len)
{
    j->p = (const uint8_t *)bytes;
    j->end = j->p + len;
    j->depth = 0;
}

/* 跳过空白；返回当前字符（<0 表示输入结束）。*/
int rkj_peek(rk_json_t *j);

/* 期望并消费一个字面字符 '{' '}' '[' ']' ',' ':'；失败返回 ERR_FIELD。*/
rkj_err_t rkj_expect(rk_json_t *j, char c);

/*
 * 解析一个 JSON 字符串（含两侧引号）。
 *   out==NULL：仅校验并跳过（用于未知键名/未知值）。
 *   out!=NULL：解码（处理 \\ \" \/ \b \f \n \r \t \uXXXX 含代理对）后写入 out，
 *              out_len 写出字节数（不含 NUL，NUL 由本函数补写）。
 *   cap 为 out 缓冲的字节容量（含 NUL 位置；解码后长度 > cap-1 → ERR_SIZE，
 *   原始字节或转义序列构成非法 UTF-8 → ERR_TRUNCATED_CP。
 */
rkj_err_t rkj_read_string(rk_json_t *j, char *out, size_t cap, size_t *out_len);

/*
 * 解析数字。is_integer 区分整数字面量（无小数/指数部分）与浮点；
 * 整数超 int64 范围 → ERR_FIELD（我们的字段最大 2^53-1，越界即非法）。
 */
rkj_err_t rkj_read_number(rk_json_t *j, bool *is_integer, int64_t *ival, double *dval);

rkj_err_t rkj_read_bool(rk_json_t *j, bool *out);
rkj_err_t rkj_expect_null(rk_json_t *j);

/*
 * 跳过任意一个 JSON 值（对象/数组递归受深度限制；字符串/数字/字面量校验语法）。
 * 用于 additionalProperties 允许的未知字段（值仍计入深度限制，§3 前向兼容规则）。
 */
rkj_err_t rkj_skip_value(rk_json_t *j);

/* 顶层值结束后必须只剩空白；否则 ERR_FIELD。*/
rkj_err_t rkj_expect_eof(rk_json_t *j);

#endif /* RK_JSON_H */
