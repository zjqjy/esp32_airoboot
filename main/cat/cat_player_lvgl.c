/*
 * S4-C/D: 哭泣猫跳舞播放引擎 — LVGL 版
 * 语义 1:1 移植自 S3-B/C cat_player.c (git 历史可回溯), 渲染从自研活动带 DMA
 * 换成 LVGL 控件树:
 *   猫     = lv_image, 帧源 = cat_lvgl_assets.c 的 RGB565 零拷贝描述符
 *   帧推进 = lv_timer, 周期 = 当前帧素材时序 delay_ms (等价 S3 的 fi_due 时刻表)
 *   舞步   = 30ms lv_timer 按 sin 时间函数挪动控件 (等价 S3 的 dance_offset)
 *   交互   = 15ms lv_timer 原始 touch_read, "350ms 静默凑次数延时提交"原样保留
 *   泪滴   = 12 颗 lv_obj 色块粒子, 30ms 定时器积分重力 (等价 S3 的 parts[])
 *   灯效   = WS2812 泪滴蓝呼吸跟舞步节拍, 彩蛋快闪 (led_beat 原样)
 * 所有定时器回调都跑在 LVGL 任务里 (lv_timer_handler), 无需加锁。
 */
#include <math.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "touch.h"
#include "led_status.h"
#include "cat_lvgl_assets.h"
#include "cat_player_lvgl.h"

static const char *TAG = "cat_lvgl";

/* 双击轮换表 (3 只), [3] 趴地哭 = 彩蛋专属, 不参与轮换 */
static const cat_anim_lvgl_t *const ANIMS[] = {
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

#define MOTION_PERIOD_MS 30     /* 舞步/粒子/灯效节拍 */
#define TOUCH_PERIOD_MS  15     /* 交互轮询限频 (等价 S3) */
#define SCREEN_W 240
#define SCREEN_H 280

static lv_obj_t *cat_img;
static lv_timer_t *frame_timer, *motion_timer, *touch_timer;
static int cur_x, cur_y;                /* 猫控件当前左上角 (含舞步偏移), 触摸命中用 */

static uint16_t cur;                    /* 当前动画下标 */
static dance_t dance = DANCE_SWAY;
static bool egg_req = false;            /* 连点猫头 → 彩蛋请求 */
static bool egg_active = false;         /* 彩蛋进行中 */
static int egg_left = 0;                /* 彩蛋剩余 GIF 帧数 */
static uint16_t saved_cur;              /* 彩蛋前的猫 + 舞步 */
static dance_t saved_dance;
static int fi;
static int64_t fps_t0;
static int frames_done;

/* 泪滴粒子 (彩蛋) */
#define PART_N 12
typedef struct { float x, y, vy; bool live; } part_t;
static part_t parts[PART_N];
static lv_obj_t *part_obj[PART_N];

/* ---------- 帧渲染层 ---------- */

static lv_color_t rgb565_color(uint16_t v)
{
    uint8_t r = (v >> 8) & 0xF8, g = (v >> 3) & 0xFC, b = (v << 3) & 0xF8;
    return lv_color_make(r | (r >> 5), g | (g >> 6), b | (b >> 5));
}

static void apply_anim(void)
{
    const cat_anim_lvgl_t *a = ANIMS[cur];
    fi = 0;                                 /* 换动画: 帧游标归零 (对齐 S3) */
    lv_obj_set_style_bg_color(lv_screen_active(), rgb565_color(a->bg_color), 0);
    lv_image_set_src(cat_img, a->frames[0]);
    lv_timer_set_period(frame_timer, a->delay_ms[0]);
}

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

/* 灯效节拍: 0..1 呼吸强度, 频率跟舞步同相; 彩蛋快闪 (S3 原样) */
static float led_beat(bool egg_active)
{
    float t = esp_timer_get_time() * 1e-6f;
    float w = 1.8f;
    if (egg_active) w = 12.0f;
    else if (dance == DANCE_BOUNCE) w = 7.0f;
    else if (dance == DANCE_GROOVE) w = 9.4f;
    else if (dance == DANCE_SWAY) w = 5.2f;
    return 0.5f + 0.5f * sinf(t * w);
}

/* ---------- 泪滴粒子 (S3 语义, 落到猫底 = 泪池消失) ---------- */

static void particle_spawn_one(float ex, float ey)
{
    for (int i = 0; i < PART_N; i++) {
        if (!parts[i].live) {
            parts[i] = (part_t){ .x = ex, .y = ey, .vy = 30.0f + (i % 3) * 25, .live = true };
            return;
        }
    }
}

/* 从两眼位置滋泪 (眼位按猫框比例估算, 萌即可) */
static void particles_rain(const cat_anim_lvgl_t *a, int n)
{
    for (int k = 0; k < n; k++) {
        float ex = cur_x + a->w * ((k & 1) ? 0.68f : 0.30f) + (k % 3) * 3 - 3;
        float ey = cur_y + a->h * 0.30f;
        particle_spawn_one(ex, ey);
    }
}

static void particles_kill_all(void)
{
    for (int i = 0; i < PART_N; i++) {
        parts[i].live = false;
        lv_obj_add_flag(part_obj[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------- 定时器回调 ---------- */

static void frame_timer_cb(lv_timer_t *t)
{
    const cat_anim_lvgl_t *a = ANIMS[cur];

    /* 彩蛋结束: 回到之前的猫 + 之前的舞步 */
    if (egg_active && egg_left <= 0) {
        egg_active = false;
        cur = saved_cur;
        dance = saved_dance;
        particles_kill_all();
        apply_anim();
        ESP_LOGI(TAG, "哭完了, 继续跳");
        return;
    }

    /* 彩蛋进入: 只认"连点猫头"请求 (双击切到趴地哭 = 普通留住) */
    if (!egg_active && egg_req) {
        egg_req = false;
        egg_active = true;
        egg_left = ANIMS[ANIM_FLOORCRY]->n_frames;
        saved_cur = (cur == ANIM_FLOORCRY) ? 0 : cur;
        saved_dance = dance;
        cur = ANIM_FLOORCRY;
        dance = DANCE_STILL;
        particles_kill_all();
        apply_anim();
        ESP_LOGI(TAG, "彩蛋: 趴地哭 %d帧", egg_left);
        return;
    }

    /* 帧推进: 换 src + 按素材时序重排周期 */
    fi = (fi + 1) % a->n_frames;
    lv_image_set_src(cat_img, a->frames[fi]);
    lv_timer_set_period(t, a->delay_ms[fi]);
    if (egg_active) {
        egg_left--;
        if (fi % 8 == 0 || fi == 1) particles_rain(a, 2);
    }

    frames_done++;
    if (frames_done % 60 == 0) {
        float fps = 60.0f * 1e6f / (esp_timer_get_time() - fps_t0);
        ESP_LOGI(TAG, "fps=%.1f (%s/%s)", fps, a->name, DANCE_NAME[dance]);
        fps_t0 = esp_timer_get_time();
    }
}

static void motion_timer_cb(lv_timer_t *t)
{
    (void)t;
    const cat_anim_lvgl_t *a = ANIMS[cur];
    int dx, dy;
    dance_offset(&dx, &dy);
    cur_x = (SCREEN_W - a->w) / 2 + dx;
    cur_y = (SCREEN_H - a->h) / 2 + dy;
    lv_obj_set_pos(cat_img, cur_x, cur_y);

    /* 粒子物理: 30ms 定步长积分, 等价 S3 的 particles_step */
    float floor_y = cur_y + a->h - 5.0f;
    const float dt = MOTION_PERIOD_MS / 1000.0f;
    for (int i = 0; i < PART_N; i++) {
        part_t *p = &parts[i];
        if (!p->live) continue;
        p->vy += 420.0f * dt;
        p->y += p->vy * dt;
        if (p->y >= floor_y) {
            p->live = false;
            lv_obj_add_flag(part_obj[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_pos(part_obj[i], (int)p->x, (int)p->y);
        }
    }

    led_status_set(0, 0, (uint8_t)(led_beat(egg_active) * 255));
}

/* ---------- 交互: 延时提交点击 (S3 原样) ---------- */

static bool tap_on_head(uint16_t tx, uint16_t ty)
{
    const cat_anim_lvgl_t *a = ANIMS[cur];
    return tx >= cur_x && tx < cur_x + a->w && ty >= cur_y && ty < cur_y + a->h * 60 / 100;
}

static void touch_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint16_t tx, ty;
    static int64_t press_start;
    static bool was_down;
    static int p_cnt;
    static bool p_head;
    static int64_t p_deadline;

    bool down = touch_read(&tx, &ty);
    if (down && !was_down) {
        press_start = esp_timer_get_time();
        p_head |= tap_on_head(tx, ty);
    } else if (!down && was_down) {
        if (esp_timer_get_time() - press_start < 400 * 1000) {
            if (p_cnt < 3) p_cnt++;
            p_deadline = esp_timer_get_time() + 350 * 1000;
        }
    }
    was_down = down;

    if (p_cnt && esp_timer_get_time() > p_deadline) {
        if (!egg_active) {
            if (p_cnt >= 3 && p_head) {
                egg_req = true;
                ESP_LOGI(TAG, "连点猫头! 哭给你看 (T_T)");
            } else if (p_cnt >= 2) {
                int pos = 0;
                for (int k = 0; k < (int)N_ROT; k++)
                    if (ROTATE[k] == cur) pos = k;
                cur = ROTATE[(pos + 1) % N_ROT];
                apply_anim();
                ESP_LOGI(TAG, "双击 → %s", ANIMS[cur]->name);
            } else {
                dance = (dance + 1) % DANCE_N;
                ESP_LOGI(TAG, "单击 → 舞步 %s", DANCE_NAME[dance]);
            }
        }
        p_cnt = 0; p_head = false;
    }
}

/* ---------- 启动 ---------- */

void cat_player_lvgl_start(void)
{
    const cat_anim_lvgl_t *a = ANIMS[cur];

    lvgl_port_lock(0);

    lv_obj_set_style_bg_color(lv_screen_active(), rgb565_color(a->bg_color), 0);

    cat_img = lv_image_create(lv_screen_active());

    /* 彩蛋泪滴: 2x2 色块, 预建 12 颗藏起来 (S3 的 PART_N 池) */
    for (int i = 0; i < PART_N; i++) {
        part_obj[i] = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(part_obj[i]);
        lv_obj_set_size(part_obj[i], 2, 2);
        lv_obj_set_style_bg_color(part_obj[i], lv_color_make(90, 180, 235), 0);
        lv_obj_set_style_bg_opa(part_obj[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(part_obj[i], LV_OBJ_FLAG_HIDDEN);
    }

    frame_timer = lv_timer_create(frame_timer_cb, 50, NULL);
    motion_timer = lv_timer_create(motion_timer_cb, MOTION_PERIOD_MS, NULL);
    touch_timer = lv_timer_create(touch_timer_cb, TOUCH_PERIOD_MS, NULL);

    apply_anim();                       /* 首帧 + 底色 + 帧周期 */
    cur_x = (SCREEN_W - a->w) / 2;
    cur_y = (SCREEN_H - a->h) / 2;
    lv_obj_set_pos(cat_img, cur_x, cur_y);
    fps_t0 = esp_timer_get_time();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "哭泣猫上线 (LVGL): %s (%dx%d, %d帧), 舞步 %s",
             a->name, a->w, a->h, a->n_frames, DANCE_NAME[dance]);
}
