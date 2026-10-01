/*
 * mic_recorder.c — S6-C 触摸录音机 (单击屏幕录音 / 再击播放)
 * 界面: 全屏录音机 (状态大字 + 实时电平条), 单击 = 录音/播放切换
 * 分工: worker 任务做音频 (录音入 PSRAM / 回放), LVGL 定时器只做
 *   触摸检测与 UI 刷新 — 回放阻塞不拖渲染, 触摸轮询不占 worker
 * 半双工: 回放期间停麦 (bsp_audio_in_stop), 防啸叫与自触发
 * 单段上限 10s (PSRAM 320KB), 到顶自动待播; 回放软件增益 ×8
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "touch.h"
#include "bsp/audio/bsp_audio_in.h"
#include "bsp/audio/bsp_audio_out.h"
#include "cat/cat_player_lvgl.h"
#include "cat_lvgl_assets.h"
#include "mic_recorder.h"

static const char *TAG = "mic_rec";

/* ---- 共享状态 (worker 独写, LVGL 定时器只读; volatile 单写者够用) ---- */
typedef enum { R_IDLE, R_REC, R_READY, R_PLAY } rec_state_t;
static volatile rec_state_t s_state = R_IDLE;
static volatile uint32_t s_rec_ms;      /* 本段已录时长 */
static volatile int32_t  s_rms;         /* 实时电平 (UI 电平条) */
static volatile bool     s_cmd;         /* 触摸定时器 → worker 切换请求 */

/* ---- 录音参数 ---- */
#define REC_CHUNK      160              /* 10ms @16kHz */
#define REC_MAX_FRAMES (MIC_SAMPLE_RATE * 10)   /* 单段上限 10s */
#define REC_GAIN       8                /* 回放软件增益 (叠加 GAIN=12dB 硬件增益) */

static int16_t *s_rec;                  /* PSRAM 录音缓冲 */

/* ---------- worker: 音频状态机 ---------- */

static void playback(size_t n)
{
    if (n == 0) {
        return;
    }
    ESP_LOGI(TAG, "回放 %lu ms", (unsigned long)((uint64_t)n * 1000 / MIC_SAMPLE_RATE));
    for (size_t i = 0; i < n; i++) {
        int32_t v = (int32_t)s_rec[i] * REC_GAIN;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        s_rec[i] = (int16_t)v;
    }
    while (bsp_audio_out_busy()) {      /* 避让 S5 自检正在播的段 */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_ERROR_CHECK(bsp_audio_out_start());
    for (size_t off = 0; off < n; off += 1024) {
        size_t take = (n - off > 1024) ? 1024 : n - off;
        if (bsp_audio_out_write(&s_rec[off], take, 2000) != ESP_OK) break;
    }
    bsp_audio_out_drain(3000);
    bsp_audio_out_stop();
}

static void rec_worker(void *arg)
{
    int16_t chunk[REC_CHUNK];
    size_t n = 0;                       /* 本段已录帧数 */

    while (1) {
        if (s_cmd) {
            s_cmd = false;
            if (s_state == R_IDLE) {
                bsp_audio_in_start();   /* 重开麦并清旧缓冲 */
                n = 0;
                s_rec_ms = 0;
                s_state = R_REC;
                ESP_LOGI(TAG, "开始录音");
            } else if (s_state == R_REC) {
                bsp_audio_in_stop();    /* 半双工: 回放期间停麦 */
                s_state = R_PLAY;
                playback(n);
                s_state = R_IDLE;
            } else if (s_state == R_READY) {
                s_state = R_PLAY;
                playback(n);
                s_state = R_IDLE;
            }
            /* R_PLAY 中忽略连点 */
        }

        if (s_state == R_REC) {
            if (bsp_audio_in_read(chunk, REC_CHUNK, 20) == ESP_OK && n < REC_MAX_FRAMES) {
                memcpy(&s_rec[n], chunk, sizeof(chunk));
                n += REC_CHUNK;
                int64_t sum2 = 0;
                for (int i = 0; i < REC_CHUNK; i++) {
                    sum2 += (int64_t)chunk[i] * chunk[i];
                }
                s_rms = (int32_t)sqrtf((float)sum2 / REC_CHUNK);
                s_rec_ms = (uint32_t)((uint64_t)n * 1000 / MIC_SAMPLE_RATE);
            }
            if (n >= REC_MAX_FRAMES) {
                bsp_audio_in_stop();
                s_state = R_READY;
                s_rms = 0;
                ESP_LOGI(TAG, "10s 上限, 单击播放");
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* ---------- LVGL: 触摸检测 + UI 刷新 ---------- */

/* 界面专用字库: lv_font_conv 生成 (黑体 16px, ASCII + 界面汉字子集).
 * 内置 source_han 只收常用 1000 字, 缺 麦/录/风/单/击/播/回 —— 模拟器渲染抓到后换的自定义字库.
 * 改界面文案后需重新生成: tools 见 .em/discussion/20261001-mic-in/ 试用记录. */
LV_FONT_DECLARE(rec_16);
#define REC_FONT &rec_16

/* 状态 → 猫动画 (S3 哭泣猫素材零拷贝复用): 黑底猫融进纯黑屏, 白底猫配白卡当"拍立得"反差梗 */
static const cat_anim_lvgl_t *const REC_CATS[] = {
    &CAT_A04_PEACH_BOWL,     /* R_IDLE  淡定吃碗 */
    &CAT_A03_PEACH_LOOK,     /* R_REC   停下抬头盯你 (在听) */
    &CAT_A01_PEACHGOMA_CRY,  /* R_PLAY  听到自己录音吓哭 (白底拍立得) */
    &CAT_A02_PEACH_FLOORCRY, /* R_READY 录满 10s 趴地崩溃 (白底拍立得) */
};
#define CAT_SCALE 154                  /* LVGL 256 = 1.0x → 220px 素材缩到 ~132px */

static lv_obj_t *s_cat, *s_catbg, *s_tdot, *s_st, *s_tm;
static const cat_anim_lvgl_t *s_cat_anim;
static int s_fi;
static int64_t s_frame_due;

static lv_color_t rgb565_to_lv(uint16_t v)
{
    return lv_color_make(((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3);
}

/* 状态切换 → 换猫 + 文案; 录音态每 tick 刷新计时 */
static void apply_state_ui(rec_state_t st)
{
    static const struct { const char *st_text, *tm_text; uint32_t dot; } MAP[] = {
        [R_IDLE]  = { "单击屏幕", "开始录音", 0x3A3A3A },
        [R_REC]   = { "录音中",   "",         0x00FF00 },
        [R_PLAY]  = { "回放中",   "",         0x3A3A3A },
        [R_READY] = { "已录 10 s", "单击播放", 0xFFD60A },
    };
    const cat_anim_lvgl_t *a = REC_CATS[st];

    s_cat_anim = a;
    s_fi = 0;
    s_frame_due = 0;
    lv_image_set_scale(s_cat, CAT_SCALE);
    lv_image_set_src(s_cat, a->frames[0]);
    if (a->bg_color != 0x0000) {                 /* 白底猫 → 拍立得白卡 */
        lv_obj_set_size(s_catbg, a->w * CAT_SCALE / 256 + 16,
                        a->h * CAT_SCALE / 256 + 16);
        lv_obj_set_style_bg_color(s_catbg, rgb565_to_lv(a->bg_color), 0);
        lv_obj_align(s_catbg, LV_ALIGN_CENTER, 0, 10);
        lv_obj_remove_flag(s_catbg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_catbg, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_st, MAP[st].st_text);
    lv_label_set_text(s_tm, MAP[st].tm_text);
    lv_obj_set_style_bg_color(s_tdot, lv_color_hex(MAP[st].dot), 0);
    if (st == R_REC || st == R_READY) {
        lv_obj_remove_flag(s_tdot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_tdot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* 触摸: 上升沿 + 300ms 冷却 → 请求切换 */
    uint16_t tx, ty;
    static bool was_down;
    static int64_t last_tap;
    bool down = touch_read(&tx, &ty);
    if (down && !was_down && esp_timer_get_time() - last_tap > 300 * 1000) {
        s_cmd = true;
        last_tap = esp_timer_get_time();
    }
    was_down = down;

    /* 状态切换 (首 tick 必然触发: 初值取不属于任何状态的哨兵) */
    static rec_state_t last_state = (rec_state_t)-1;
    if (s_state != last_state) {
        last_state = s_state;
        apply_state_ui(last_state);
    }
    if (s_state == R_REC) {                      /* 录音计时每 tick 刷新 */
        static char buf[16];
        snprintf(buf, sizeof(buf), "%lu.%lu s",
                 (unsigned long)(s_rec_ms / 1000), (unsigned long)((s_rec_ms % 1000) / 100));
        lv_label_set_text(s_tm, buf);
    }

    /* 猫帧推进 (按素材 GIF 时序) */
    if (s_cat_anim != NULL) {
        int64_t now = esp_timer_get_time();
        if (now >= s_frame_due) {
            s_fi = (s_fi + 1) % s_cat_anim->n_frames;
            lv_image_set_src(s_cat, s_cat_anim->frames[s_fi]);
            s_frame_due = now + (int64_t)s_cat_anim->delay_ms[s_fi] * 1000;
        }
    }
}

/* ---------- 启动 ---------- */

void mic_recorder_start(void)
{
    ESP_ERROR_CHECK(bsp_audio_in_init());       /* 幂等 */
    ESP_ERROR_CHECK(bsp_audio_out_init());      /* 幂等 (S5 自检可能已装) */

    s_rec = heap_caps_malloc(REC_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_rec == NULL) {
        ESP_LOGE(TAG, "PSRAM 不足, 录音机不可用");
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(rec_worker, "mic_rec", 4096, NULL, 5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "worker 创建失败");
        return;
    }

    lvgl_port_lock(0);
    cat_player_lvgl_pause();            /* 停猫动画与触摸定时器, 触摸由录音机独占 */

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);   /* xiaozhi dark: 纯黑 */
    lv_obj_set_style_text_font(scr, REC_FONT, 0);

    /* 顶部状态栏 (xiaozhi 式: 左标题 + 右指示点) */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, 34);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "录音机");
    lv_obj_set_style_text_color(title, lv_color_hex(0x9A9A9A), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);
    s_tdot = lv_obj_create(bar);
    lv_obj_remove_style_all(s_tdot);
    lv_obj_set_size(s_tdot, 8, 8);
    lv_obj_set_style_radius(s_tdot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_tdot, LV_OPA_COVER, 0);
    lv_obj_align(s_tdot, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_flag(s_tdot, LV_OBJ_FLAG_HIDDEN);

    /* 猫舞台: 白底猫垫"拍立得"白卡, 黑底猫直接融进黑屏 */
    s_catbg = lv_obj_create(scr);
    lv_obj_remove_style_all(s_catbg);
    lv_obj_set_style_radius(s_catbg, 18, 0);
    lv_obj_set_style_bg_opa(s_catbg, LV_OPA_COVER, 0);
    lv_obj_align(s_catbg, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_flag(s_catbg, LV_OBJ_FLAG_HIDDEN);
    s_cat = lv_image_create(scr);
    lv_obj_align(s_cat, LV_ALIGN_CENTER, 0, 10);

    s_st = lv_label_create(scr);
    lv_label_set_text(s_st, "单击屏幕");
    lv_obj_set_style_text_color(s_st, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_st, LV_ALIGN_TOP_MID, 0, 40);

    s_tm = lv_label_create(scr);
    lv_label_set_text(s_tm, "开始录音");
    lv_obj_set_style_text_color(s_tm, lv_color_hex(0x8A8A8A), 0);
    lv_obj_align(s_tm, LV_ALIGN_TOP_MID, 0, 62);

    /* 底部 assistant 气泡 (xiaozhi: bubble #222222) */
    lv_obj_t *bub = lv_obj_create(scr);
    lv_obj_remove_style_all(bub);
    lv_obj_set_size(bub, 148, 34);
    lv_obj_align(bub, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(bub, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bub, 17, 0);
    lv_obj_t *bt = lv_label_create(bub);
    lv_label_set_text(bt, "单击=录音/播放");
    lv_obj_set_style_text_color(bt, lv_color_hex(0xDDDDDD), 0);
    lv_obj_center(bt);

    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
    lv_timer_create(ui_timer_cb, 50, NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "录音机界面就绪: 单击=录音/播放 (回放增益 x%d, 单段上限 10s)", REC_GAIN);
}
