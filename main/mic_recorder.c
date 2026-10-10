/*
 * mic_recorder.c — S6-C 触摸录音机 → S7 录音回放+识别共存
 * 界面: 全屏录音机 (状态大字 + 实时电平条), 单击 = 录音/播放切换
 * 停录收尾 (S7 方案 B): 尾巴 chunk 先上识别服务器 → 喇叭本地回放 → X-Final
 * 分工: worker 任务做音频 (录音入 PSRAM / 回放), LVGL 定时器只做
 *   触摸检测与 UI 刷新 — 回放阻塞不拖渲染, 触摸轮询不占 worker
 * 半双工: 回放期间停麦 (bsp_audio_in_stop), 防啸叫与自触发
 * 单段上限 60s (PSRAM 1.92MB); 回放旁路增益 x8, 峰值过大自动降 (不碰 s_rec)
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "touch.h"
#include "bsp/audio/bsp_audio_in.h"
#include "bsp/audio/bsp_audio_out.h"
#include "cat/cat_player_lvgl.h"
#include "cat_lvgl_assets.h"
#include "wifi_link.h"
#include "mic_recorder.h"

static const char *TAG = "mic_rec";

/* ---- 共享状态 (worker 独写, LVGL 定时器只读; volatile 单写者够用) ---- */
typedef enum { R_IDLE, R_REC, R_READY, R_PLAY, R_SONG } rec_state_t;
static volatile rec_state_t s_state = R_IDLE;
static volatile uint32_t s_rec_ms;      /* 本段已录时长 */
static volatile int32_t  s_rms;         /* 实时电平 (UI 电平条) */
static volatile bool     s_cmd;         /* 触摸定时器 → worker 切换请求 */
static volatile bool     s_song_cmd;    /* 长按: 空闲=播歌曲 / 歌曲中=停止 */

/* ---- 录音参数 ---- */
#define REC_CHUNK      160              /* 10ms @16kHz */
#define REC_MAX_FRAMES (MIC_SAMPLE_RATE * 60)   /* 单段上限 60s (PSRAM 1.92MB) */
#define REC_GAIN       8                /* 回放软件增益 (叠加 GAIN=12dB 硬件增益) */
#define STREAM_CHUNK   8000             /* 流式块 250ms, POST 耗时 << 环形缓冲 512ms */

static int16_t *s_rec;                  /* PSRAM 录音缓冲 */

/* 实时示波条: worker 每块(10ms)写入峰值幅度, UI 定时器读出画线 —— 待机即可视的拾音验证 */
#define WAVE_N 120
static volatile int16_t s_wave[WAVE_N];
static volatile int s_wave_idx;

/* ---------- worker: 音频状态机 ---------- */

#if CONFIG_S6_MIC_SERIAL_DUMP
/* 录音结束经串口导出 PCM: ===XZ_PCM 头 + base64 + 尾, PC 端 tools/serial_recorder.py 收流存 WAV */
static void serial_dump_pcm(size_t frames)
{
    size_t bytes = frames * sizeof(int16_t);
    size_t b64_len = ((bytes + 2) / 3) * 4 + 4;
    char *b64 = heap_caps_malloc(b64_len, MALLOC_CAP_SPIRAM);
    if (b64 == NULL) {
        ESP_LOGW(TAG, "dump 内存不足, 跳过导出");
        return;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, b64_len, &olen,
                              (const unsigned char *)s_rec, bytes) != 0) {
        heap_caps_free(b64);
        return;
    }
    printf("\n===XZ_PCM v1 n=%u sr=16000 ch=1 bits=16===\n", (unsigned)frames);
    for (size_t off = 0; off < olen; off += 4000) {
        size_t take = (olen - off > 4000) ? 4000 : olen - off;
        fwrite(b64 + off, 1, take, stdout);
        printf("\n");
    }
    printf("===XZ_PCM_END===\n");
    fflush(stdout);
    heap_caps_free(b64);
    ESP_LOGI(TAG, "已串口导出 %u 帧 PCM", (unsigned)frames);
}
#endif

static size_t s_sent;                   /* 已流式发出的帧数 (ASR 关时恒 0) */
static bool s_final_done;               /* 本段识别已触发, 防重复 */

#if CONFIG_S6_WIFI_ASR
/* 流式: 录音中每 250ms 把新片段 POST 给 PC (X-Chunk), 终止时 X-Final 触发识别 */
static char s_asr_text[48];
static volatile bool s_asr_new;

static bool asr_send_chunk(const int16_t *frames, size_t n)
{
    return wifi_link_post(CONFIG_S6_ASR_URL, (const char *)frames,
                          n * sizeof(int16_t), "X-Chunk", "1",
                          NULL, 0, 500) == ESP_OK;
}

static void finalize_asr(void)
{
    snprintf(s_asr_text, sizeof(s_asr_text), "识别中");
    s_asr_new = true;
    char resp[192] = { 0 };
    esp_err_t err = wifi_link_post(CONFIG_S6_ASR_URL, "", 0,
                                   "X-Final", "1", resp, sizeof(resp), 30000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "识别请求失败: %s", esp_err_to_name(err));
        snprintf(s_asr_text, sizeof(s_asr_text), "识别失败");
        s_asr_new = true;
        return;
    }
    const char *p = strstr(resp, "\"text\":\"");
    if (p == NULL) {
        ESP_LOGW(TAG, "响应异常: %s", resp);
        snprintf(s_asr_text, sizeof(s_asr_text), "识别失败");
        s_asr_new = true;
        return;
    }
    p += 8;
    const char *e = strchr(p, '"');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len > sizeof(s_asr_text) - 1) len = sizeof(s_asr_text) - 1;
    memcpy(s_asr_text, p, len);
    s_asr_text[len] = 0;
    s_asr_new = true;
    ESP_LOGI(TAG, "识别: %s", s_asr_text);
}
#endif

/* 回放: 旁路增益 — 写 I2S 时缩放, s_rec 保持原样 (S7: 尾巴 PCM 还要发识别).
 * 峰值会削顶时自动降增益; 峰值<500 基本等于没录到声音 */
static void playback(size_t n)
{
    if (n == 0) {
        return;
    }
    int16_t pk = 0;
    for (size_t i = 0; i < n; i++) {
        int16_t a = (s_rec[i] >= 0) ? s_rec[i] : (int16_t)-s_rec[i];
        if (a > pk) pk = a;
    }
    int gain = REC_GAIN;
    if (pk > 0 && pk * REC_GAIN > 30000) {
        gain = 30000 / pk;
        if (gain < 1) gain = 1;
    }
    ESP_LOGI(TAG, "回放 %lu ms, 峰值 %d, 增益 x%d",
             (unsigned long)((uint64_t)n * 1000 / MIC_SAMPLE_RATE), pk, gain);
    while (bsp_audio_out_busy()) {      /* 避让 S5 自检正在播的段 */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_ERROR_CHECK(bsp_audio_out_start());
    int16_t buf[1024];
    for (size_t off = 0; off < n; off += 1024) {
        size_t take = (n - off > 1024) ? 1024 : n - off;
        for (size_t i = 0; i < take; i++) {
            int32_t v = (int32_t)s_rec[off + i] * gain;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            buf[i] = (int16_t)v;
        }
        if (bsp_audio_out_write(buf, take, 2000) != ESP_OK) break;
    }
    bsp_audio_out_drain(3000);
    bsp_audio_out_stop();
}

/* 停录收尾统一入口 (S7 方案 B): 尾巴先上识别服务器 → 喇叭本地回放 → X-Final.
 * 回放旁路增益不碰 s_rec, 所以先发尾巴还是先回放都不伤数据; 识别文字回 UI 气泡 */
static void finish_and_play(size_t n)
{
#if CONFIG_S6_WIFI_ASR
    if (wifi_link_got_ip() && n > s_sent) {
        asr_send_chunk(&s_rec[s_sent], n - s_sent);
        s_sent = n;
    }
#endif
    s_state = R_PLAY;
    playback(n);
    s_state = R_IDLE;                   /* UI 先回待机, 文字到了再进气泡 */
#if CONFIG_S6_WIFI_ASR
    if (wifi_link_got_ip() && !s_final_done) {
        finalize_asr();
        s_final_done = true;
    }
#endif
}

/* 长按播放 /assets/song.wav (16k/mono/16bit PCM): 播放中再长按停止.
 * 不用 bsp_audio_out_play_wav: 它阻塞整曲无法打断, 这里自管流式 + 每块查停 */
static void song_play(void)
{
    bsp_assets_mount();                 /* 幂等; 覆盖自检关闭时未挂载的情况 */
    FILE *f = fopen("/assets/song.wav", "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "没找到 /assets/song.wav (tools/song2wav.py + gen_tts_wavs.py fat + flash_assets.bat)");
        return;
    }
    uint8_t hdr[44];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVEfmt ", 8) ||
        (hdr[20] | (hdr[21] << 8)) != 1 ||                    /* PCM */
        (hdr[22] | (hdr[23] << 8)) != 1 ||                    /* mono */
        (hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24)) != MIC_SAMPLE_RATE ||
        (hdr[34] | (hdr[35] << 8)) != 16) {
        fclose(f);
        ESP_LOGW(TAG, "song.wav 参数不符: 需 PCM/mono/16k/16bit");
        return;
    }
    uint32_t total = (hdr[40] | (hdr[41] << 8) | (hdr[42] << 16) | (uint32_t)hdr[43] << 24) / 2;
    ESP_LOGI(TAG, "歌曲 %lu s, 长按停止", (unsigned long)(total / MIC_SAMPLE_RATE));
    s_state = R_SONG;                   /* 半双工: 歌曲期间停麦 */
    bsp_audio_in_stop();
    while (bsp_audio_out_busy()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_ERROR_CHECK(bsp_audio_out_start());
    static int16_t buf[1024];
    size_t r, played = 0;
    while (!s_song_cmd && (r = fread(buf, sizeof(int16_t), 1024, f)) > 0) {
        if (bsp_audio_out_write(buf, r, 2000) != ESP_OK) break;
        played += r;
    }
    fclose(f);
    bsp_audio_out_drain(3000);
    bsp_audio_out_stop();
    s_song_cmd = false;                 /* 停止请求就地消费, 防回待机后误触发重播 */
    s_state = R_IDLE;
    ESP_LOGI(TAG, "歌曲结束: 播了 %lu/%lu 帧", (unsigned long)played, (unsigned long)total);
}

static void rec_worker(void *arg)
{
    int16_t chunk[REC_CHUNK];
    size_t n = 0;                       /* 本段已录帧数 */
    int64_t next_rms_log = 0;

    while (1) {
        if (s_cmd) {
            s_cmd = false;
            if (s_state == R_IDLE) {
                bsp_audio_in_start();   /* 重开麦并清旧缓冲 */
                n = 0;
                s_sent = 0;
                s_final_done = false;
                s_rec_ms = 0;
                s_state = R_REC;
                next_rms_log = 0;
                ESP_LOGI(TAG, "开始录音 (实时传输)");
            } else if (s_state == R_REC) {
                bsp_audio_in_stop();    /* 半双工: 停麦 */
#if CONFIG_S6_MIC_SERIAL_DUMP
                if (!wifi_link_got_ip()) {
                    serial_dump_pcm(n); /* 离线兜底: 串口导出 */
                }
#endif
                finish_and_play(n);     /* 尾巴→喇叭回放→识别 (S7) */
            } else if (s_state == R_READY) {
                finish_and_play(n);     /* 60s 到顶后单击: 同收尾流程 */
            }
            /* R_PLAY 中忽略连点 */
        }

        if (s_song_cmd) {               /* 长按: 空闲开播 (歌曲内部把同标志当停止) */
            s_song_cmd = false;
            if (s_state == R_IDLE) {
                song_play();
            }
        }

        /* 待机常开拾音: 示波条待机也能看, 1.4mA 代价可忽略 (回放态除外, 半双工) */
        if (s_state == R_IDLE && !bsp_audio_in_running()) {
            bsp_audio_in_start();
        }
        if (!bsp_audio_in_running()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (bsp_audio_in_read(chunk, REC_CHUNK, 20) != ESP_OK) continue;

        int16_t pk = 0;
        int64_t sum2 = 0;
        for (int i = 0; i < REC_CHUNK; i++) {
            int16_t a = (chunk[i] >= 0) ? chunk[i] : (int16_t)-chunk[i];
            if (a > pk) pk = a;
            sum2 += (int64_t)chunk[i] * chunk[i];
        }
        s_wave[s_wave_idx] = pk;
        s_wave_idx = (s_wave_idx + 1) % WAVE_N;
        s_rms = (int32_t)sqrtf((float)sum2 / REC_CHUNK);

        if (s_state == R_REC) {
            if (n < REC_MAX_FRAMES) {
                memcpy(&s_rec[n], chunk, sizeof(chunk));
                n += REC_CHUNK;
                s_rec_ms = (uint32_t)((uint64_t)n * 1000 / MIC_SAMPLE_RATE);
                if (esp_timer_get_time() >= next_rms_log) {
                    next_rms_log = esp_timer_get_time() + 250000;
                    ESP_LOGI(TAG, "[rec] %lu ms RMS=%ld 已传 %lu KB",
                             (unsigned long)s_rec_ms, (long)s_rms,
                             (unsigned long)(s_sent * 2 / 1024));
                }
            }
#if CONFIG_S6_WIFI_ASR
            /* 实时流式: 攒满 250ms 就发, POST 耗时 << 环形缓冲, 不丢采样 */
            if (n - s_sent >= STREAM_CHUNK / (int)sizeof(int16_t)) {
                if (!asr_send_chunk(&s_rec[s_sent], n - s_sent)) {
                    ESP_LOGW(TAG, "chunk 发送失败, 本段跳过");
                }
                s_sent = n;
            }
#endif
            if (n >= REC_MAX_FRAMES) {
                bsp_audio_in_stop();
                s_state = R_READY;      /* 等单击回放 (不突然外放 60s) */
#if CONFIG_S6_MIC_SERIAL_DUMP
                if (!wifi_link_got_ip()) serial_dump_pcm(n);
#endif
#if CONFIG_S6_WIFI_ASR
                if (wifi_link_got_ip() && n > s_sent) {
                    asr_send_chunk(&s_rec[s_sent], n - s_sent);   /* 数据先落服务器 */
                    s_sent = n;
                }
#endif
                ESP_LOGI(TAG, "60s 上限, 单击回放");
            }
        }
    }
}

/* ---------- LVGL: 触摸检测 + UI 刷新 ---------- */

/* 界面专用字库: lv_font_conv 生成 (黑体 16px, ASCII + 界面汉字子集).
 * 内置 source_han 只收常用 1000 字, 缺 麦/录/风/单/击/播/回 —— 模拟器渲染抓到后换的自定义字库.
 * 改界面文案后需重新生成: tools 见 .em/discussion/20261001-mic-in/ 试用记录. */
LV_FONT_DECLARE(rec_16);
LV_FONT_DECLARE(lv_font_montserrat_14);   /* 顶栏音符图标 (FontAwesome 子集) */
#define REC_FONT &rec_16

/* 状态 → 猫动画 (S3 哭泣猫素材零拷贝复用): 黑底猫融进纯黑屏, 白底猫配白卡当"拍立得"反差梗 */
static const cat_anim_lvgl_t *const REC_CATS[] = {
    &CAT_A04_PEACH_BOWL,     /* R_IDLE  淡定吃碗 */
    &CAT_A03_PEACH_LOOK,     /* R_REC   停下抬头盯你 (在听) */
    &CAT_A01_PEACHGOMA_CRY,  /* R_PLAY  听到自己录音吓哭 (白底拍立得) */
    &CAT_A02_PEACH_FLOORCRY, /* R_READY 录满 60s 趴地崩溃 (白底拍立得) */
    &CAT_A03_PEACH_LOOK,     /* R_SONG  盯着听歌 */
};
#define CAT_SCALE 154                  /* LVGL 256 = 1.0x → 220px 素材缩到 ~132px */

static lv_obj_t *s_cat, *s_catbg, *s_tdot, *s_st, *s_tm;
static const cat_anim_lvgl_t *s_cat_anim;
static int s_fi;
static int64_t s_frame_due;

static lv_obj_t *s_wave_line;
static lv_point_precise_t s_wave_pts[WAVE_N];
static lv_obj_t *s_bub_text;

static lv_color_t rgb565_to_lv(uint16_t v)
{
    return lv_color_make(((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3);
}

/* 状态切换 → 换猫 + 文案; 录音态每 tick 刷新计时 */
static void apply_state_ui(rec_state_t st)
{
    static const struct { const char *st_text, *tm_text; uint32_t dot; } MAP[] = {
        [R_IDLE]  = { "单击屏幕", "开始录音", 0x3A3A3A },
        [R_REC]   = { "录音中",   "",         0x3DDC84 },
        [R_PLAY]  = { "回放中",   "",         0x8B93FF },
        [R_READY] = { "已录 60 s", "",        0xFFD60A },
        [R_SONG]  = { "播放中",   "长按停",    0x8B93FF },
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
    if (st != R_IDLE) {
        lv_obj_remove_flag(s_tdot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_tdot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* 触摸: 短按抬起=切换, 长按 600ms=歌曲, 300ms 冷却 */
    uint16_t tx, ty;
    static bool was_down, long_fired;
    static int64_t press_t0, last_tap;
    int64_t now = esp_timer_get_time();
    bool down = touch_read(&tx, &ty);
    if (down && !was_down) {
        press_t0 = now;
        long_fired = false;
    } else if (down && !long_fired && now - press_t0 > 600 * 1000) {
        s_song_cmd = true;              /* 长按: 播/停歌曲 */
        long_fired = true;
    } else if (!down && was_down && !long_fired &&
               now - last_tap > 300 * 1000) {
        s_cmd = true;                   /* 短按: 录音/回放 */
        last_tap = now;
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

    /* 实时示波条: 最近 1.2s 峰值包络, 说话即起伏 (待机可视的拾音验证) */
    for (int i = 0; i < WAVE_N; i++) {
        int idx = (s_wave_idx + i) % WAVE_N;        /* 旧 → 新 */
        int amp = s_wave[idx];
        if (amp > 8000) amp = 8000;
        s_wave_pts[i].x = i * 220 / (WAVE_N - 1);
        s_wave_pts[i].y = 18 - amp * 18 / 8000;     /* 静音贴底线, 越响越高 */
    }
    lv_obj_invalidate(s_wave_line);

    /* PC 识别结果 → 气泡显示 (识别文字可能含生僻字, 换内置思源黑体) */
#if CONFIG_S6_WIFI_ASR
    if (s_asr_new) {
        s_asr_new = false;
#if CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
        lv_obj_set_style_text_font(s_bub_text, &lv_font_source_han_sans_sc_16_cjk, 0);
#endif
        lv_label_set_text(s_bub_text, s_asr_text);
    }
#endif

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
    ESP_LOGI(TAG, "[boot] mic_recorder_start 进入");
    ESP_ERROR_CHECK(bsp_audio_in_init());       /* 幂等 */
    ESP_ERROR_CHECK(bsp_audio_out_init());      /* 幂等 (S5 自检可能已装) */
    ESP_LOGI(TAG, "[boot] 音频驱动就绪");

    s_rec = heap_caps_malloc(REC_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_rec == NULL) {
        ESP_LOGE(TAG, "PSRAM 不足, 录音机不可用");
        return;
    }
    ESP_LOGI(TAG, "[boot] 录音缓冲分配 OK");

    BaseType_t ok = xTaskCreatePinnedToCore(rec_worker, "mic_rec", 4096, NULL, 5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "worker 创建失败");
        return;
    }
    ESP_LOGI(TAG, "[boot] worker 已创建");

    lvgl_port_lock(0);
    ESP_LOGI(TAG, "[boot] lvgl lock 拿到, 暂停猫定时器");
    cat_player_lvgl_pause();            /* 停猫动画与触摸定时器, 触摸由录音机独占 */

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);   /* xiaozhi dark: 纯黑 */
    lv_obj_set_style_text_font(scr, REC_FONT, 0);

    /* 顶部状态栏 chip (v5 精修: 内嵌面板 + 音符图标, 参照 music demo/c3-mini) */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 224, 30);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x232229), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 12, 0);
    lv_obj_t *ic = lv_label_create(bar);
    lv_label_set_text(ic, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0x6F8AF6), 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "录音机");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 30, 0);
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
    lv_obj_align(s_catbg, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(s_catbg, LV_OBJ_FLAG_HIDDEN);
    s_cat = lv_image_create(scr);
    lv_obj_align(s_cat, LV_ALIGN_CENTER, 0, -10);

    /* 实时示波条 (待机常开拾音, 说话即起伏) */
    s_wave_line = lv_line_create(scr);
    for (int i = 0; i < WAVE_N; i++) {
        s_wave_pts[i].x = i * 220 / (WAVE_N - 1);
        s_wave_pts[i].y = 18;
    }
    lv_line_set_points(s_wave_line, s_wave_pts, WAVE_N);
    lv_obj_set_pos(s_wave_line, 10, 210);
    lv_obj_set_style_line_color(s_wave_line, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_line_width(s_wave_line, 2, 0);

    s_st = lv_label_create(scr);
    lv_label_set_text(s_st, "单击屏幕");
    lv_obj_set_style_text_color(s_st, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_st, LV_ALIGN_TOP_MID, 0, 40);

    s_tm = lv_label_create(scr);
    lv_label_set_text(s_tm, "开始录音");
    lv_obj_set_style_text_color(s_tm, lv_color_hex(0xDADDE2), 0);
    lv_obj_align(s_tm, LV_ALIGN_TOP_MID, 0, 62);

    /* 底部 assistant 气泡 (xiaozhi: bubble #222222) */
    lv_obj_t *bub = lv_obj_create(scr);
    lv_obj_remove_style_all(bub);
    lv_obj_set_size(bub, 148, 34);
    lv_obj_align(bub, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(bub, lv_color_hex(0x2A2933), 0);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bub, 17, 0);
    lv_obj_t *bt = lv_label_create(bub);
    s_bub_text = bt;
    lv_label_set_text(bt, "单击=录音/播放");
    lv_obj_set_style_text_color(bt, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bt);

    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
    ESP_LOGI(TAG, "[boot] 界面控件创建完成, 加载动画启动");
    lv_timer_create(ui_timer_cb, 50, NULL);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "[boot] 录音机界面创建完成");

    ESP_LOGI(TAG, "录音机就绪: 单击=录音/回放, 长按=歌曲 (增益 x%d, 上限 60s)", REC_GAIN);
}
