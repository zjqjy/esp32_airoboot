/*
 * S3-B/C: 哭泣猫跳舞播放引擎
 * 素材: cat_assets.c (tools/anim_gif2c.py 生成: 4bpp + 16色RGB565, bake底色)
 * 渲染: 复用 S2-A 逐行大端 DMA 路径, 只推猫活动带 (前后两帧行并集)
 * 节奏: 渲染循环拉满(舞步是时间函数, 越快越顺滑), GIF 帧按素材自身时序推进
 * 交互: "延时提交"——点击后 350ms 静默凑次数一次定案:
 *       单击=切舞步  双击=切猫(留下)  连点猫头(≥3次)=趴地哭彩蛋(滋泪+快闪灯)
 * 灯效: WS2812 泪滴蓝呼吸跟舞步节拍, 彩蛋时快闪
 */
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lcd_init.h"
#include "touch.h"
#include "led_status.h"
#include "cat_assets.h"
#include "cat_player.h"

static const char *TAG = "cat_player";

/* 双击轮换表 (3 只), [3] 趴地哭 = 彩蛋专属, 不参与轮换 */
static const cat_anim_t *const ANIMS[] = {
    &CAT_A03_PEACH_LOOK,
    &CAT_A04_PEACH_BOWL,
    &CAT_A01_PEACHGOMA_CRY,
    &CAT_A02_PEACH_FLOORCRY,
};
#define ANIM_N (sizeof(ANIMS) / sizeof(ANIMS[0]))
#define ANIM_FLOORCRY 3
static const uint8_t ROTATE[] = { 0, 1, 2 };
#define N_ROT (sizeof(ROTATE) / sizeof(ROTATE[0]))

typedef enum { DANCE_STILL, DANCE_SWAY, DANCE_BOUNCE, DANCE_GROOVE, DANCE_N } dance_t;
static const char *const DANCE_NAME[DANCE_N] = { "still", "sway", "bounce", "groove" };

static uint8_t *row_buf;        /* 480B DMA 行缓冲 (大端字节对, 同 LCD_GetLineBuf 格式) */
static uint16_t cur;            /* 当前动画下标 */
static dance_t dance = DANCE_SWAY;
static bool egg_req = false;    /* 连点猫头 → 主循环进彩蛋 */

/* ---------- 泪滴粒子 (彩蛋) ---------- */
#define PART_N 12
typedef struct { float x, y, vy; bool live; } part_t;
static part_t parts[PART_N];

static void particle_spawn_one(float ex, float ey)
{
    for (int i = 0; i < PART_N; i++) {
        if (!parts[i].live) {
            parts[i] = (part_t){ .x = ex, .y = ey, .vy = 30.0f + (i % 3) * 25, .live = true };
            return;
        }
    }
}

/* 从两眼位置滋泪 (眼位按猫框比例估算, 萌即可, 不追帧级对齐) */
static void particles_rain(const cat_anim_t *a, int cx, int cy, int n)
{
    for (int k = 0; k < n; k++) {
        float ex = cx + a->w * ((k & 1) ? 0.68f : 0.30f) + (k % 3) * 3 - 3;
        float ey = cy + a->h * 0.30f;
        particle_spawn_one(ex, ey);
    }
}

static void particles_step(float dt, const cat_anim_t *a, int cx, int cy)
{
    float floor_y = cy + a->h - 5.0f;           /* 落到猫底=泪池, 消失 */
    for (int i = 0; i < PART_N; i++) {
        part_t *p = &parts[i];
        if (!p->live) continue;
        p->vy += 420.0f * dt;                   /* 重力 */
        p->y += p->vy * dt;
        if (p->y >= floor_y) p->live = false;
    }
}

static void particles_kill_all(void)
{
    for (int i = 0; i < PART_N; i++) parts[i].live = false;
}

/* ---------- 舞步运动层 ---------- */
static void dance_offset(int *dx, int *dy)
{
    float t = esp_timer_get_time() * 1e-6f;
    *dx = *dy = 0;
    switch (dance) {
    case DANCE_SWAY:   *dx = (int)(sinf(t * 5.2f) * 8); break;
    case DANCE_BOUNCE: *dy = -(int)(fabsf(sinf(t * 7.0f)) * 14); break;
    case DANCE_GROOVE: *dx = (int)(sinf(t * 6.8f) * 10);
                       *dy = -(int)(fabsf(sinf(t * 9.4f)) * 16); break;
    default: break;
    }
}

/* 灯效节拍: 0..1 呼吸强度, 频率跟舞步同相; 彩蛋快闪 */
static float led_beat(bool egg_active)
{
    float t = esp_timer_get_time() * 1e-6f;
    float w = 1.8f;                             /* still: 慢呼吸 */
    if (egg_active) w = 12.0f;
    else if (dance == DANCE_BOUNCE) w = 7.0f;
    else if (dance == DANCE_GROOVE) w = 9.4f;
    else if (dance == DANCE_SWAY) w = 5.2f;
    return 0.5f + 0.5f * sinf(t * w);
}

static void cat_rect(const cat_anim_t *a, int *cx, int *cy)
{
    int dx, dy;
    dance_offset(&dx, &dy);
    *cx = (LCD_W - a->w) / 2 + dx;
    *cy = (LCD_H - a->h) / 2 + dy;
}

static inline void fill_row_bg(uint8_t *p, uint8_t h, uint8_t l)
{
    for (int x = 0; x < LCD_W; x++) { p[2 * x] = h; p[2 * x + 1] = l; }
}

/* ---------- 帧渲染: 组装 [y0..y1] 每行并推屏 ----------
 * 前提: LCD_SetWindow 已把窗口开到 (0,y0)-(LCD_W-1,y1),
 * ST7789 写显存自动逐行回绕, 之后只需连续 SendPixels */
static void render_band(const cat_anim_t *a, int fi, int y0, int y1)
{
    int cx, cy;
    cat_rect(a, &cx, &cy);
    const uint8_t *ib = a->idx[fi];
    uint16_t bg = a->bg_color;
    uint8_t bgH = bg >> 8, bgL = bg & 0xFF;
    const uint16_t tear = ((90 & 0xF8) << 8) | ((180 & 0xFC) << 3) | (235 >> 3);

    int row_stride = (a->w + 1) / 2;                /* 4bpp 行字节数 */
    int sx0 = cx < 0 ? 0 : cx;                      /* 猫在屏上的水平裁剪 */
    int sx1 = cx + a->w > LCD_W ? LCD_W : cx + a->w;

    for (int y = y0; y <= y1; y++) {
        fill_row_bg(row_buf, bgH, bgL);
        if (y >= cy && y < cy + a->h) {             /* 该行穿过猫身 → 展开 4bpp */
            const uint8_t *irow = ib + (size_t)(y - cy) * row_stride;
            for (int sx = sx0; sx < sx1; sx++) {
                int ix = sx - cx;
                uint8_t b = irow[ix >> 1];
                uint16_t c = a->palette[(ix & 1) ? (b & 0xF) : (b >> 4)];
                row_buf[2 * sx] = c >> 8;           /* ST7789 要大端: 高字节先 */
                row_buf[2 * sx + 1] = c & 0xFF;
            }
        }
        for (int i = 0; i < PART_N; i++) {          /* 泪滴粒子盖在猫上 */
            const part_t *p = &parts[i];
            if (p->live && (int)p->y == y) {
                int px = (int)p->x;
                if (px >= 0 && px + 1 < LCD_W) {
                    row_buf[2 * px] = tear >> 8;      row_buf[2 * px + 1] = tear & 0xFF;
                    row_buf[2 * px + 2] = tear >> 8;  row_buf[2 * px + 3] = tear & 0xFF;
                }
            }
        }
        LCD_SendPixels((const uint16_t *)row_buf, LCD_W);
    }
}

/* 全屏重铺 (切换动画/启动时): 底色一次铺满 */
static void push_full_bg(const cat_anim_t *a)
{
    uint16_t bg = a->bg_color;
    fill_row_bg(row_buf, bg >> 8, bg & 0xFF);
    LCD_SetWindow(0, 0, LCD_W - 1, LCD_H - 1);
    for (int y = 0; y < LCD_H; y++) LCD_SendPixels((const uint16_t *)row_buf, LCD_W);
}

/* ---------- 交互: 延时提交点击 ----------
 * 每次点击后等 350ms 静默凑次数, 一次定案, 消除单击/双击/连点竞态 */
static int64_t now_us(void) { return esp_timer_get_time(); }

static bool tap_on_head(uint16_t tx, uint16_t ty)
{
    int cx, cy;
    const cat_anim_t *a = ANIMS[cur];
    cat_rect(a, &cx, &cy);
    return tx >= cx && tx < cx + a->w && ty >= cy && ty < cy + a->h * 60 / 100;
}

static int64_t press_start;
static bool was_down;
static int p_cnt;
static bool p_head;
static int64_t p_deadline;

static void touch_poll(bool egg_active)
{
    uint16_t tx, ty;
    bool down = touch_read(&tx, &ty);           /* true = 手指按着 */

    if (down && !was_down) {                    /* 按下沿: 记坐标 */
        press_start = now_us();
        p_head |= tap_on_head(tx, ty);
    } else if (!down && was_down) {             /* 抬起沿: 短按计一次 */
        if (now_us() - press_start < 400 * 1000) {
            if (p_cnt < 3) p_cnt++;
            p_deadline = now_us() + 350 * 1000;
        }
    }
    was_down = down;

    if (p_cnt && now_us() > p_deadline) {       /* 静默期满 → 提交 */
        if (!egg_active) {
            if (p_cnt >= 3 && p_head) {
                egg_req = true;                 /* 连点猫头 → 彩蛋 */
                ESP_LOGI(TAG, "连点猫头! 哭给你看 (T_T)");
            } else if (p_cnt >= 2) {
                int pos = 0;                    /* 双击: 轮换表内切猫 (趴地哭不在表里) */
                for (int k = 0; k < (int)N_ROT; k++)
                    if (ROTATE[k] == cur) pos = k;
                cur = ROTATE[(pos + 1) % N_ROT];
                ESP_LOGI(TAG, "双击 → %s", ANIMS[cur]->name);
            } else {
                dance = (dance + 1) % DANCE_N;
                ESP_LOGI(TAG, "单击 → 舞步 %s", DANCE_NAME[dance]);
            }
        }
        p_cnt = 0; p_head = false;
    }
}

/* ---------- 主循环 ---------- */
void cat_player_run(void)
{
    row_buf = heap_caps_malloc(LCD_W * 2, MALLOC_CAP_DMA);
    if (!row_buf) {
        ESP_LOGE(TAG, "行缓冲分配失败");
        return;
    }
    ESP_ERROR_CHECK(touch_init());

    const cat_anim_t *a = ANIMS[cur];
    ESP_LOGI(TAG, "哭泣猫上线: %s (%dx%d, %d帧), 舞步 %s", a->name, a->w, a->h,
             a->n_frames, DANCE_NAME[dance]);
    push_full_bg(a);

    int fi = 0;
    int prev_y0 = 0, prev_y1 = LCD_H - 1;       /* 首帧覆盖全屏, 后面只推活动带 */
    bool egg = false;                           /* 彩蛋进行中 */
    uint16_t saved_cur = cur;
    dance_t saved_dance = dance;
    int egg_left = 0;                           /* 彩蛋剩余 GIF 帧数 */
    int64_t fi_due = 0;                         /* 下一 GIF 帧推进时刻 */
    int64_t last_poll = 0;
    int64_t fps_t0 = now_us();
    int frames_done = 0;

    while (1) {
        /* a 保持上一轮值 → 用 a != ANIMS[cur] 统一检测"本轮 cur 是否变了" */

        /* 彩蛋结束: 回到之前的猫 + 之前的舞步 */
        if (egg && egg_left <= 0) {
            egg = false;
            cur = saved_cur;
            dance = saved_dance;
            particles_kill_all();
            ESP_LOGI(TAG, "哭完了, 继续跳");
        }

        /* 彩蛋进入: 只认"连点猫头"请求 (双击切到趴地哭=普通留住) */
        if (!egg && egg_req) {
            egg_req = false;
            egg = true;
            egg_left = ANIMS[ANIM_FLOORCRY]->n_frames;
            saved_cur = (cur == ANIM_FLOORCRY) ? 0 : cur;
            saved_dance = dance;
            cur = ANIM_FLOORCRY;
            dance = DANCE_STILL;
            particles_kill_all();
            ESP_LOGI(TAG, "彩蛋: 趴地哭 %d帧", egg_left);
        }

        /* 切换检测 (双击 / 彩蛋进出): 重置帧游标 + 铺新底 + 清残影带 */
        if (a != ANIMS[cur]) {
            a = ANIMS[cur];
            fi = 0; fi_due = 0;
            push_full_bg(a);
            prev_y0 = 0; prev_y1 = LCD_H - 1;
        }

        int cx, cy;
        cat_rect(a, &cx, &cy);

        /* 本帧行带 = 当前猫行区间 ∪ 上一帧行区间 (清掉上一帧残影) */
        int cy0 = cy < 0 ? 0 : cy;
        int cy1 = cy + a->h - 1 >= LCD_H ? LCD_H - 1 : cy + a->h - 1;
        int y0 = cy0 < prev_y0 ? cy0 : prev_y0;
        int y1 = cy1 > prev_y1 ? cy1 : prev_y1;
        LCD_SetWindow(0, y0, LCD_W - 1, y1);
        render_band(a, fi, y0, y1);
        prev_y0 = cy0; prev_y1 = cy1;

        /* 灯效: 泪滴蓝呼吸跟节拍 */
        led_status_set(0, 0, (uint8_t)(led_beat(egg) * 255));

        frames_done++;
        if (frames_done % 60 == 0) {
            float fps = 60.0f * 1e6f / (now_us() - fps_t0);
            ESP_LOGI(TAG, "fps=%.1f (%s/%s)", fps, a->name, DANCE_NAME[dance]);
            fps_t0 = now_us();
        }

        /* GIF 帧推进: 按素材自身时序, 与渲染节奏解耦 */
        int64_t now = now_us();
        if (now >= fi_due) {
            fi = (fi + 1) % a->n_frames;
            fi_due = now + a->delay_ms[fi] * 1000;
            if (egg) {
                egg_left--;                     /* 剩余量按 GIF 帧计 */
                if (fi % 8 == 0 || fi == 1) particles_rain(a, cx, cy, 2);
            }
        }

        /* 交互轮询: 按时间限频 (~15ms 响应, 不给 I2C 加压) */
        if (now - last_poll >= 15 * 1000) {
            last_poll = now;
            touch_poll(egg);
        }

        vTaskDelay(1);                          /* tick=1ms @1000Hz, 让出 CPU */
    }
}
