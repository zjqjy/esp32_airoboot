/*
 * bsp_audio_out.c — MAX98357A 驱动实现 (S5-A/B/C)
 * 引脚宏占位 BCLK=12/LRCK=13/DIN=14/SD=15, 实物接线后登记 BSP_PLAN 引脚表.
 * GAIN 接 GND = 12dB (硬件定音量); DIN 上 I2S1 mono 模式硬件复制到左右声道,
 * 所以 SD 引脚选左/右声道都能出声 (声道选择失效风险消除).
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "bsp_audio_out.h"
#include "bsp/audio/audio_pins.h"

static const char *TAG = "audio_out";

#ifndef M_PI
#define M_PI 3.14159265358979f
#endif

/* ---- I2S / SD ---- */
static i2s_chan_handle_t s_tx;
static bool s_enabled;                  /* I2S 通道 enable 状态 */
static TaskHandle_t s_task;

/* ---- 环形缓冲 (SPSC: 生产者=API 调用者, 消费者=audio_task) ---- */
static int16_t s_ring[AUDIO_RING_FRAMES];
static volatile uint32_t s_head;        /* 写位置 (生产者独写) */
static volatile uint32_t s_tail;        /* 读位置 (消费者独写) */
#define RING_MASK (AUDIO_RING_FRAMES - 1)

static inline uint32_t ring_used(void)
{
    uint32_t h = s_head, t = s_tail;
    return (h - t) & RING_MASK;
}

/* ---- 播放状态机 ---- */
typedef enum { IDLE, STREAMING, DRAINING } play_state_t;
static volatile play_state_t s_state = IDLE;

/* ---- assets 只读挂载 ---- */
static wl_handle_t s_wl = WL_INVALID_HANDLE;
#define ASSETS_MOUNT "/assets"
#define ASSETS_PART  "assets"

/* ---------- audio_task: 消费者 ---------- */

static void task_move_chunk(void)
{
    static int16_t chunk[AUDIO_DMA_FRAMES];
    uint32_t used = ring_used();
    uint32_t n = (used < AUDIO_DMA_FRAMES) ? used : AUDIO_DMA_FRAMES;

    if (n == 0) {
        if (s_state == DRAINING) {          /* 排空完成 */
            s_state = IDLE;
        }
        memset(chunk, 0, sizeof(chunk));    /* underrun → 静音帧 */
        n = AUDIO_DMA_FRAMES;
    } else {
        uint32_t t = s_tail;
        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = s_ring[(t + i) & RING_MASK];
        }
        s_tail = (t + n) & RING_MASK;
        if (n < AUDIO_DMA_FRAMES) {         /* 不足一块补静音 */
            memset(&chunk[n], 0, (AUDIO_DMA_FRAMES - n) * sizeof(int16_t));
        }
    }
    size_t bytes = AUDIO_DMA_FRAMES * sizeof(int16_t), written = 0;
    if (i2s_channel_write(s_tx, chunk, bytes, &written,
                          100) != ESP_OK || written != bytes) {
        ESP_LOGW(TAG, "i2s write fail");
    }
}

static void audio_task(void *arg)
{
    while (1) {
        if (s_enabled) {
            task_move_chunk();
        }
        vTaskDelay(pdMS_TO_TICKS(5));       /* 90ms 硬件缓冲, 5ms 巡检绰绰有余 */
    }
}

/* ---------- 生产者 ---------- */

esp_err_t bsp_audio_out_write(const int16_t *samples, size_t n, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (n) {
        uint32_t free_n = AUDIO_RING_FRAMES - 1 - ring_used();
        if (free_n == 0) {
            if (esp_timer_get_time() > deadline) {
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        uint32_t h = s_head;
        uint32_t put = (n < free_n) ? (uint32_t)n : free_n;
        for (uint32_t i = 0; i < put; i++) {
            s_ring[(h + i) & RING_MASK] = samples[i];
        }
        s_head = (h + put) & RING_MASK;
        samples += put;
        n -= put;
    }
    return ESP_OK;
}

/* ---------- 停播 / 静音 ---------- */

esp_err_t bsp_audio_out_mute(bool mute)
{
    return gpio_set_level(AUDIO_SD_PIN, mute ? 0 : 1);
}

esp_err_t bsp_audio_out_start(void)
{
    if (!s_enabled) {
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable");
        s_enabled = true;
        vTaskDelay(pdMS_TO_TICKS(AUDIO_SD_QUIET_MS));
    }
    return bsp_audio_out_mute(false);
}

esp_err_t bsp_audio_out_stop(void)
{
    s_state = DRAINING;
    esp_err_t err = bsp_audio_out_drain(3000);
    bsp_audio_out_mute(true);               /* 先断功放 (防大直流) */
    if (s_enabled) {
        i2s_channel_disable(s_tx);
        s_enabled = false;
    }
    s_head = s_tail = 0;
    return err;
}

esp_err_t bsp_audio_out_drain(uint32_t timeout_ms)
{
    s_state = DRAINING;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (s_state == DRAINING) {
        if (esp_timer_get_time() > deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

bool bsp_audio_out_busy(void)
{
    return s_state != IDLE || ring_used() != 0;
}

/* ---------- 播放原语 ---------- */

esp_err_t bsp_audio_out_play_sine(uint32_t duration_ms)
{
    enum { TONE_N = 160 };                  /* 440Hz @16kHz: 每周期 ≈ 36.36 帧, 160 帧 = 10ms 整 */
    static int16_t tone[TONE_N];
    for (int i = 0; i < TONE_N; i++) {
        tone[i] = (int16_t)(12000.0f * sinf(2.0f * (float)M_PI * 440.0f * i / AUDIO_SAMPLE_RATE));
    }
    bsp_audio_out_start();                  /* stop 后重播: 幂等使能 */
    s_state = STREAMING;
    uint32_t loops = duration_ms / (TONE_N * 1000 / AUDIO_SAMPLE_RATE);
    for (uint32_t i = 0; i < loops; i++) {
        esp_err_t err = bsp_audio_out_write(tone, TONE_N, 1000);
        if (err != ESP_OK) return err;
    }
    return bsp_audio_out_drain(2000);
}

esp_err_t bsp_audio_out_play_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "wav 不存在: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    /* 44 字节标准头: 校验 RIFF/WAVE/PCM/采样率/位深 */
    uint8_t hdr[44];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVEfmt ", 8)) {
        fclose(f);
        ESP_LOGE(TAG, "非标准 WAV 头: %s", path);
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint16_t fmt = hdr[20] | (hdr[21] << 8);
    uint16_t ch = hdr[22] | (hdr[23] << 8);
    uint32_t rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    uint16_t bits = hdr[34] | (hdr[35] << 8);
    if (fmt != 1 || ch != 1 || rate != AUDIO_SAMPLE_RATE || bits != 16) {
        fclose(f);
        ESP_LOGE(TAG, "WAV 参数不符 (需 PCM/mono/%dk/16bit): %s", AUDIO_SAMPLE_RATE / 1000, path);
        return ESP_ERR_NOT_SUPPORTED;
    }

    bsp_audio_out_start();                  /* stop 后重播: 幂等使能 */
    s_state = STREAMING;
    enum { BUF_N = 1024 };
    static int16_t buf[BUF_N];
    esp_err_t ret = ESP_OK;
    size_t r;
    while ((r = fread(buf, sizeof(int16_t), BUF_N, f)) > 0) {
        ret = bsp_audio_out_write(buf, r, 2000);
        if (ret != ESP_OK) break;
    }
    fclose(f);
    esp_err_t d = bsp_audio_out_drain(3000);
    return (ret != ESP_OK) ? ret : d;
}

/* ---------- 初始化 ---------- */

esp_err_t bsp_audio_out_init(void)
{
    /* 1. SD 引脚: 默认拉低 = 功放关断 0.6µA */
    gpio_config_t sd = {
        .pin_bit_mask = 1ULL << AUDIO_SD_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&sd), TAG, "sd gpio");
    bsp_audio_out_mute(true);

    /* 2. I2S1 TX: 16kHz/16bit/标准 Philips, mono (硬件复制左右声道) */
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan.dma_desc_num = AUDIO_DMA_DESC;
    chan.dma_frame_num = AUDIO_DMA_FRAMES;
    chan.auto_clear = true;                 /* disable 时 DMA 自动清零, 收尾干净 */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, NULL), TAG, "chan");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,        /* MAX98357A 不需要 MCLK */
            .bclk = AUDIO_BCLK_PIN,
            .ws = AUDIO_LRCK_PIN,
            .dout = AUDIO_DIN_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { 0, 0, 0 },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "std init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable");
    s_enabled = true;

    /* 3. 时钟稳定后再放行功放 (turn-on 7.5ms, 留 250ms 富余防爆音) */
    vTaskDelay(pdMS_TO_TICKS(AUDIO_SD_QUIET_MS));
    bsp_audio_out_mute(false);

    /* 4. audio_task: 核1 优先级20 (ARCHITECTURE §3 实时音频位) */
    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio_out", 4096,
                                            NULL, 20, &s_task, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");

    ESP_LOGI(TAG, "MAX98357 ready (I2S1 %dk/16bit/mono, BCLK=%d LRCK=%d DIN=%d SD=%d)",
             AUDIO_SAMPLE_RATE / 1000, AUDIO_BCLK_PIN, AUDIO_LRCK_PIN,
             AUDIO_DIN_PIN, AUDIO_SD_PIN);
    return ESP_OK;
}

/* ---------- assets 分区只读挂载 (S5-C) ---------- */

esp_err_t bsp_assets_mount(void)
{
    if (s_wl != WL_INVALID_HANDLE) return ESP_OK;
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 2,
        .format_if_mount_failed = false,    /* 镜像由 tools/gen_assets_fat.py 预生成 */
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_ro(ASSETS_MOUNT, ASSETS_PART, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "assets 挂载失败 (%s): 台词播放不可用, 其余功能不受影响", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "assets 只读挂载 OK → %s", ASSETS_MOUNT);
    return ESP_OK;
}
