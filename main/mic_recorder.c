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

static lv_obj_t *s_arc, *s_dot, *s_st, *s_tm;

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

    /* 状态 → 圆环颜色 / 中心文字 / 红点 / 电平 */
    static char buf[32];
    uint32_t ring, tcol;
    int val, dot_on;
    const char *st, *tm = "";
    switch (s_state) {
    case R_REC:
        ring = 0xFF5A5A; tcol = 0xE8EAED;
        snprintf(buf, sizeof(buf), "%lu.%lu s",
                 (unsigned long)(s_rec_ms / 1000), (unsigned long)((s_rec_ms % 1000) / 100));
        tm = buf;
        st = "录音中";
        val = (s_rms > 1000) ? 100 : s_rms / 10;
        dot_on = 1;
        break;
    case R_READY:
        ring = 0xFFC94D; tcol = 0xFFC94D;
        st = "已录 10 s";
        tm = "单击播放";
        val = 100;
        dot_on = 0;
        break;
    case R_PLAY:
        ring = 0x53D18A; tcol = 0x53D18A;
        st = "回放中";
        val = 0;
        dot_on = 0;
        break;
    default:
        ring = 0x222A34; tcol = 0xE8EAED;
        st = "单击屏幕";
        tm = "开始录音";
        val = 0;
        dot_on = 0;
        break;
    }
    lv_arc_set_value(s_arc, val);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(ring), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_st, lv_color_hex(tcol), 0);
    lv_label_set_text(s_st, st);
    lv_label_set_text(s_tm, tm);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(ring), 0);
    if (dot_on) {
        lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1116), 0);
    lv_obj_set_style_text_font(scr, REC_FONT, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "录 音 机");
    lv_obj_set_style_text_color(title, lv_color_hex(0x8A93A0), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    /* 中央圆环: 背圈=轨道, 指示弧=实时电平, 内嵌状态+计时 */
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 156, 156);
    lv_obj_align(s_arc, LV_ALIGN_CENTER, 0, -16);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x222A34), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x222A34), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_arc, lv_color_hex(0x161B22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_arc, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    s_dot = lv_obj_create(scr);         /* 录音红点 (几何绘制, 不依赖字形) */
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 10, 10);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_align(s_dot, LV_ALIGN_CENTER, 0, -38);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

    s_st = lv_label_create(scr);
    lv_label_set_text(s_st, "单击屏幕");
    lv_obj_set_style_text_color(s_st, lv_color_hex(0xE8EAED), 0);
    lv_obj_align(s_st, LV_ALIGN_CENTER, 0, -12);

    s_tm = lv_label_create(scr);
    lv_label_set_text(s_tm, "开始录音");
    lv_obj_set_style_text_color(s_tm, lv_color_hex(0x8A93A0), 0);
    lv_obj_align(s_tm, LV_ALIGN_CENTER, 0, 12);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "单击=录音/播放  重启退出");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x566070), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);

    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
    lv_timer_create(ui_timer_cb, 50, NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "录音机界面就绪: 单击=录音/播放 (回放增益 x%d, 单段上限 10s)", REC_GAIN);
}
