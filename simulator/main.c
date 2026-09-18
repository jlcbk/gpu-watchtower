/**
 * @file main.c
 * rig-lookout-sim — Mac 端模拟器（P2a）。
 *
 * 与固件编译同一份 shared UI/解析代码（CMakeLists 相对路径引用）；渲染
 * 400x300 I1，经 SDL 渲染器回读为逻辑帧（1=黑，rk_frame_t），再导出为标准
 * 1-bit 灰度 PNG（P2b 修复：流式 CRC32 + 打包位图，字节确定，~15KB/帧；
 * P2a 旧版 IDAT CRC 恒 0 被严格解码器拒收、8-bit stored 虚胖 120KB）。
 *
 * P2a 用法（8 帧 = 2 页 × 4 态）：
 *   rig-lookout-sim --fixture protocol/stats.normal.json --state normal  \
 *                   --page 1 --png ../artifacts/p2a/page1-normal.png
 * 状态语义（PLAN §5）：normal=fixtures 原样；offline=固件侧合成（数值全
 * "--" + 离线横幅）；nodriver=stats.nodriver.json（gpu.driver=false）；alarm=
 * 固件侧温度阈值触发（--alarm-temp 注入显示值）。
 *
 * 无头运行：SDL_VIDEODRIVER=dummy（出帧后自动退出，不开窗）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lvgl.h"
#include "rk_font_unifont16.h"
#include "rk_frame.h"
#include "rk_stats.h"
#include "rk_ui.h"
#include LV_SDL_INCLUDE_PATH

#define SIM_HOR_RES 400
#define SIM_VER_RES 300
#define TREND_PTS 120 /* 60min @30s（PLAN §5 走势缓冲显示切片） */

typedef enum { ST_NORMAL = 0, ST_OFFLINE, ST_NODRIVER, ST_ALARM } sim_state_t;

typedef struct {
    const char *fixture;   /* --fixture */
    sim_state_t state;     /* --state */
    rk_page_t page;        /* --page */
    const char *png;       /* --png */
    const char *last_online; /* --last-online */
    const char *clock_text;  /* --clock */
    double alarm_temp;     /* --alarm-temp */
    int alarm_cpu;         /* --alarm-src cpu */
    long trend_seed;       /* --trend-seed */
    const char *batt;      /* --batt */
    int busy;              /* --busy */
    const char *env;       /* --env */
} sim_opts_t;

static void usage(const char *prog)
{
    printf("usage: %s [options]\n"
           "  --fixture <stats.json>   rig-stats snapshot JSON (required)\n"
           "  --state <normal|offline|nodriver|alarm>  device state (default normal)\n"
           "  --page <1|2>             page (default 1)\n"
           "  --png <out.png>          export logical mono frame as PNG\n"
           "  --last-online <HH:MM>    offline banner text (default --clock)\n"
           "  --clock <HH:MM>          bottom-right clock (default from ts+08)\n"
           "  --alarm-temp <C>         alarm board temperature (default max(gpu,cpu))\n"
           "  --alarm-src <gpu|cpu>    alarm source label (default gpu)\n"
           "  --trend-seed <N>         deterministic trend walk seed (default 42)\n"
           "  --batt <TEXT>            footer battery slot, e.g. \"4.12V\" / \"USB\" (default off)\n"
           "  --busy                   show the rendering busy tag (default off)\n"
           "  --env <TEXT>             footer env slot, e.g. \"24.9C 36%\" (default off)\n",
           prog);
}

static void parse_args(int argc, char **argv, sim_opts_t *o)
{
    memset(o, 0, sizeof(*o));
    o->state = ST_NORMAL;
    o->page = RK_PAGE_GPU;
    o->trend_seed = 42;
    o->alarm_temp = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--fixture") == 0 && i + 1 < argc) o->fixture = argv[++i];
        else if (strcmp(a, "--state") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            if (strcmp(s, "normal") == 0) o->state = ST_NORMAL;
            else if (strcmp(s, "offline") == 0) o->state = ST_OFFLINE;
            else if (strcmp(s, "nodriver") == 0) o->state = ST_NODRIVER;
            else if (strcmp(s, "alarm") == 0) o->state = ST_ALARM;
            else { fprintf(stderr, "[sim] ERROR: --state must be normal|offline|nodriver|alarm\n"); exit(2); }
        }
        else if (strcmp(a, "--page") == 0 && i + 1 < argc) {
            o->page = (atoi(argv[++i]) == 2) ? RK_PAGE_SYS : RK_PAGE_GPU;
        }
        else if (strcmp(a, "--png") == 0 && i + 1 < argc) o->png = argv[++i];
        else if (strcmp(a, "--last-online") == 0 && i + 1 < argc) o->last_online = argv[++i];
        else if (strcmp(a, "--clock") == 0 && i + 1 < argc) o->clock_text = argv[++i];
        else if (strcmp(a, "--alarm-temp") == 0 && i + 1 < argc) o->alarm_temp = atof(argv[++i]);
        else if (strcmp(a, "--alarm-src") == 0 && i + 1 < argc) {
            o->alarm_cpu = (strcmp(argv[++i], "cpu") == 0);
        }
        else if (strcmp(a, "--trend-seed") == 0 && i + 1 < argc) o->trend_seed = atol(argv[++i]);
        else if (strcmp(a, "--batt") == 0 && i + 1 < argc) o->batt = argv[++i];
        else if (strcmp(a, "--busy") == 0) o->busy = 1;
        else if (strcmp(a, "--env") == 0 && i + 1 < argc) o->env = argv[++i];
        else { fprintf(stderr, "[sim] ERROR: unknown/incomplete argument: %s\n", a); usage(argv[0]); exit(2); }
    }
    if (o->fixture == NULL) {
        fprintf(stderr, "[sim] ERROR: --fixture is required\n");
        usage(argv[0]);
        exit(2);
    }
}

/* ---------- fixture 读取 + rk_stats 解析（与固件同一条解析路径） ---------- */

static int load_fixture(const char *path, rk_stats_t *out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "[sim] ERROR: cannot open fixture '%s'\n", path);
        return -1;
    }
    static uint8_t buf[RK_STATS_JSON_MAX_BYTES + 1];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    rk_parse_result_t r = rk_stats_parse(buf, n, out);
    if (r != RK_PARSE_OK) {
        fprintf(stderr, "[sim] ERROR: fixture '%s' rejected by rk_stats_parse (code %d)\n",
                path, (int)r);
        return -1;
    }
    return 0;
}

/* ---------- 走势合成（确定性 LCG；终点=当前 GPU 温度） ----------
 * P2a 展示用；真机走势来自固件 12h 环形缓冲（PLAN §5），P2b 换 fixtures 全集。 */

static void synth_trend(const rk_stats_t *s, long seed, float *trend, int len)
{
    double end = 65.0;
    if (s->gpu.temp_c.present) end = s->gpu.temp_c.value;
    else if (s->cpu.temp_c.present) end = s->cpu.temp_c.value;

    uint32_t st = (uint32_t)seed * 2654435761u + 1u;
    double v = end - 8.0;
    if (v < 55.0) v = 55.0;
    for (int i = 0; i < len; i++) {
        double k = (double)i / (double)(len - 1);
        st = st * 1664525u + 1013904223u;
        double noise = ((double)((st >> 8) & 0xFF) / 255.0) - 0.5; /* ±0.5 */
        double base = (end - 8.0) + (end - (end - 8.0)) * k;       /* 线性爬升 */
        v = base + noise * 2.0;
        if (v < 50.0) v = 50.0;
        if (v > end + 1.5) v = end + 1.5;
        trend[i] = (float)v;
    }
    trend[len - 1] = (float)end; /* 终点对齐实时值 */
}

/* ---------- 逻辑帧回读 + PNG 导出（标准 PNG；字节确定） ---------- */

static void sample_into_frame(const uint8_t *argb, int ow, int oh, rk_frame_t *f)
{
    rk_frame_clear(f, 0);
    for (int y = 0; y < SIM_VER_RES; y++) {
        int sy = (int)((long)y * oh / SIM_VER_RES);
        const uint32_t *row = (const uint32_t *)(argb + (size_t)sy * ow * 4);
        for (int x = 0; x < SIM_HOR_RES; x++) {
            int sx = (int)((long)x * ow / SIM_HOR_RES);
            uint32_t px = row[sx];
            int black = ((px & 0xFFu) < 0x80u); /* I1 渲染为纯黑/纯白 */
            if (black) rk_frame_set(f, x, y, 1);
        }
    }
}

static int capture_logical_frame(lv_display_t *disp, rk_frame_t *f)
{
    SDL_Renderer *ren = (SDL_Renderer *)lv_sdl_window_get_renderer(disp);
    if (ren == NULL) {
        fprintf(stderr, "[sim] capture: no SDL renderer\n");
        return -1;
    }
    int ow, oh;
    if (SDL_GetRendererOutputSize(ren, &ow, &oh) != 0) {
        fprintf(stderr, "[sim] capture: GetRendererOutputSize failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surf == NULL) {
        fprintf(stderr, "[sim] capture: surface alloc failed: %s\n", SDL_GetError());
        return -1;
    }
    if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch) != 0) {
        fprintf(stderr, "[sim] capture: RenderReadPixels failed: %s\n", SDL_GetError());
        SDL_FreeSurface(surf);
        return -1;
    }
    sample_into_frame((const uint8_t *)surf->pixels, ow, oh, f);
    SDL_FreeSurface(surf);
    return 0;
}

/* P2b：标准 1-bit 灰度 PNG（bit depth=1, color type=0），CRC32 全程流式计算。
 * P2a 旧版缺陷：png_chunk 的 CRC 暂存缓冲仅 512B，超限把 IDAT CRC 恒写 0 →
 * 严格解码器（macOS Read 等）拒收，且 8-bit 灰度 stored 块单帧虚胖 120KB。
 * 现 1-bit 打包 + stored zlib 仍字节确定（golden 流前提），单帧 ~15KB。 */

static uint32_t g_crc;
static void crc_reset(void) { g_crc = 0xFFFFFFFFu; }
static void crc_feed(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        g_crc ^= d[i];
        for (int k = 8; k-- > 0;) g_crc = (g_crc >> 1) ^ (0xEDB88320u & (0u - (g_crc & 1u)));
    }
}
static uint32_t crc_value(void) { return g_crc ^ 0xFFFFFFFFu; }

static void png_chunk(FILE *fp, const char *type, const uint8_t *body, uint32_t len)
{
    uint8_t hdr[8], crcb[4];
    uint32_t crc;
    hdr[0] = (uint8_t)(len >> 24); hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);  hdr[3] = (uint8_t)len;
    hdr[4] = (uint8_t)type[0]; hdr[5] = (uint8_t)type[1];
    hdr[6] = (uint8_t)type[2]; hdr[7] = (uint8_t)type[3];
    crc_reset();
    crc_feed(hdr + 4, 4); /* chunk type */
    fwrite(hdr, 1, 8, fp);
    if (len > 0) {
        fwrite(body, 1, len, fp);
        crc_feed(body, len);
    }
    crc = crc_value();
    crcb[0] = (uint8_t)(crc >> 24); crcb[1] = (uint8_t)(crc >> 16);
    crcb[2] = (uint8_t)(crc >> 8);  crcb[3] = (uint8_t)crc;
    fwrite(crcb, 1, 4, fp);
}

static uint32_t png_adler32(const uint8_t *d, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + d[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* 400x300 1-bit 灰度 PNG：0=黑 1=白，filter=None，zlib stored 块 → 字节确定 */
static int write_frame_png(const rk_frame_t *f, const char *path)
{
    static uint8_t raw[(1 + SIM_HOR_RES / 8) * SIM_VER_RES];
    static uint8_t zbuf[sizeof(raw) + 64];
    size_t raw_len = 0, z_len = 0;

    for (int y = 0; y < SIM_VER_RES; y++) {
        uint8_t b = 0;
        raw[raw_len++] = 0; /* filter: None */
        for (int x = 0; x < SIM_HOR_RES; x++) {
            int bit = rk_frame_get(f, x, y) ? 0 : 1; /* 1=黑 → 采样 0=黑 */
            b = (uint8_t)((b << 1) | bit);           /* MSB 在前 */
            if ((x & 7) == 7) raw[raw_len++] = b;
        }
    }

    /* zlib 流：stored 块（无压缩但合法、字节确定） */
    zbuf[z_len++] = 0x78; zbuf[z_len++] = 0x01;
    size_t off = 0;
    while (off < raw_len) {
        size_t blk = raw_len - off;
        if (blk > 65535) blk = 65535;
        int last = (off + blk >= raw_len);
        zbuf[z_len++] = last ? 1 : 0;
        zbuf[z_len++] = (uint8_t)(blk & 0xFF);
        zbuf[z_len++] = (uint8_t)(blk >> 8);
        zbuf[z_len++] = (uint8_t)(~blk & 0xFF);
        zbuf[z_len++] = (uint8_t)((~blk >> 8) & 0xFF);
        memcpy(zbuf + z_len, raw + off, blk);
        z_len += blk;
        off += blk;
    }
    uint32_t ad = png_adler32(raw, raw_len);
    zbuf[z_len++] = (uint8_t)(ad >> 24); zbuf[z_len++] = (uint8_t)(ad >> 16);
    zbuf[z_len++] = (uint8_t)(ad >> 8);  zbuf[z_len++] = (uint8_t)ad;

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) return -1;
    uint8_t ihdr[13];
    uint32_t w = SIM_HOR_RES, h = SIM_VER_RES;
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = 1;  /* bit depth: 1（打包灰度，0=黑 1=白） */
    ihdr[9] = 0;  /* color type: grayscale */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, fp);
    png_chunk(fp, "IHDR", ihdr, 13);
    png_chunk(fp, "IDAT", zbuf, (uint32_t)z_len);
    png_chunk(fp, "IEND", NULL, 0);
    fclose(fp);
    return 0;
}

/* ---------- main：读 fixture → 建模 → 渲染 → 抓帧 → PNG ---------- */

int main(int argc, char **argv)
{
    sim_opts_t opts;
    parse_args(argc, argv, &opts);

    rk_stats_t stats;
    if (load_fixture(opts.fixture, &stats) != 0) return 2;

    printf("[sim] rig-lookout-sim LVGL %d.%d.%d, %dx%d I1, fixture=%s state=%d page=%d\n",
           lv_version_major(), lv_version_minor(), lv_version_patch(),
           SIM_HOR_RES, SIM_VER_RES, opts.fixture, (int)opts.state, (int)opts.page);

    /* 时钟/最后在线：默认 ts+08:00 → HH:MM（模拟器确定性；真机走本地时区） */
    static char clock_buf[8], online_buf[8];
    if (opts.clock_text != NULL) {
        snprintf(clock_buf, sizeof(clock_buf), "%s", opts.clock_text);
    } else {
        long long t = (long long)stats.ts + 8LL * 3600LL;
        snprintf(clock_buf, sizeof(clock_buf), "%02lld:%02lld",
                 (t / 3600) % 24, (t / 60) % 60);
    }
    if (opts.last_online != NULL) {
        snprintf(online_buf, sizeof(online_buf), "%s", opts.last_online);
    } else {
        snprintf(online_buf, sizeof(online_buf), "%s", clock_buf);
    }

    /* 走势合成（驱动未装/离线也填充——离线冻结最后历史，nodriver 由 UI 隐藏数据线） */
    static float trend[TREND_PTS];
    synth_trend(&stats, opts.trend_seed, trend, TREND_PTS);

    /* 模型 */
    rk_ui_model_t model;
    memset(&model, 0, sizeof(model));
    model.stats = stats;
    model.have_snapshot = true;
    model.page = opts.page;
    model.trend = trend;
    model.trend_len = TREND_PTS;
    model.clock_text = clock_buf;
    model.last_online = online_buf;
    model.batt_text = opts.batt;
    model.gpu_busy = opts.busy;
    model.env_text = opts.env;
    switch (opts.state) {
        case ST_OFFLINE: model.state = RK_STATE_OFFLINE; break;
        case ST_NODRIVER: model.state = RK_STATE_NODRIVER; break;
        case ST_ALARM: {
            model.state = RK_STATE_ALARM;
            bool is_gpu = !opts.alarm_cpu;
            float t = opts.alarm_temp;
            if (t < 0.0f) {
                bool hit = rk_alarm_hit(&stats, &is_gpu, &t);
                if (!hit) { is_gpu = true; t = (float)RK_TEMP_ALARM_C + 2.0f; }
            }
            model.alarm_temp_c = t;
            model.alarm_is_gpu = is_gpu;
            break;
        }
        default: model.state = RK_STATE_NORMAL; break;
    }

    /* LVGL + SDL（无头经 SDL_VIDEODRIVER=dummy） */
    lv_init();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[sim] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    lv_display_t *disp = lv_sdl_window_create(SIM_HOR_RES, SIM_VER_RES);
    if (disp == NULL) {
        fprintf(stderr, "[sim] lv_sdl_window_create failed\n");
        SDL_Quit();
        return 1;
    }
    lv_sdl_window_set_title(disp, "rig-lookout sim");
    lv_sdl_window_set_resizeable(disp, false);

    rk_ui_init();
    rk_ui_apply(&model);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);

    int rc = 0;
    if (opts.png != NULL) {
        rk_frame_t frame;
        if (capture_logical_frame(disp, &frame) != 0) {
            fprintf(stderr, "[sim] capture FAILED\n");
            rc = 3;
        } else if (write_frame_png(&frame, opts.png) != 0) {
            fprintf(stderr, "[sim] ERROR: cannot write %s\n", opts.png);
            rc = 3;
        } else {
            printf("[sim] PNG exported: %s (%dx%d, 1=black)\n",
                   opts.png, RK_FRAME_WIDTH, RK_FRAME_HEIGHT);
        }
    }

    /* 码点核查（PLAN §8：温度符号等是否缺字——° U+00B0 / ⚠ U+26A0 / CJK 状态词
     * 「驱动未装」「离」「线」） */
    printf("[sim] codepoint coverage: degree(U+00B0)=%d warning(U+26A0)=%d "
           "qu(U+9A71)=%d li(U+79BB)=%d xian(U+7EBF)=%d zhuang(U+88C5)=%d\n",
           (int)rk_font_unifont16_covers(0x00B0),
           (int)rk_font_unifont16_covers(0x26A0),
           (int)rk_font_unifont16_covers(0x9A71),
           (int)rk_font_unifont16_covers(0x79BB),
           (int)rk_font_unifont16_covers(0x7EBF),
           (int)rk_font_unifont16_covers(0x88C5));

    lv_display_delete(disp);
    lv_sdl_quit();
    lv_deinit();
    SDL_Quit();
    return rc;
}
