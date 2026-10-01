/*
 * bsp_audio_in.c — INMP441 麦克风采集驱动实现 (S6-A/B)
 * 引脚宏占位 BCLK=9/WS=8/DIN=11, 实物接线后登记 BSP_PLAN 引脚表.
 * INMP441 = 24bit I2S 从机, 无寄存器可配: L/R 接 GND 固定左声道, 只接 3.3V.
 * 时序: Philips 标准, 数据比 WS 翻转晚 1 拍 → 32bit slot 收 MSB 恰落最高位,
 *   低 8 位为 0 → (int16_t)(raw >> 16) 直接取高 16 位 (按 16bit slot 配会截掉 MSB).
 * 溢出策略 = 丢最旧: 消费者跟不上时保最新语音, 延迟上限 = 环形缓冲 256ms.
 *   tail 双写 (消费者 pop / 生产者丢帧) 用自旋锁临界区保护.
 * 无关断脚: stop = i2s_channel_disable, 时钟停芯片自动进低功耗.
 */
#include <string.h>
#include <math.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp_audio_in.h"
#include "bsp/audio/mic_pins.h"

static const char *TAG = "audio_in";

/* ---- I2S ---- */
static i2s_chan_handle_t s_rx;
static volatile bool s_enabled;

/* ---- 环形缓冲 (SPSC + 溢出丢最旧: 生产者=audio_in_task, 消费者=read 调用者) ---- */
static int16_t s_ring[MIC_RING_FRAMES];
static volatile uint32_t s_head;        /* 写位置 (生产者独写) */
static volatile uint32_t s_tail;        /* 读位置 (消费者独写; 溢出时生产者在锁内前移) */
static volatile uint32_t s_overflows;   /* 累计丢帧 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_of_log_mark;
#define RING_MASK (MIC_RING_FRAMES - 1)

static inline uint32_t ring_used(void)
{
    uint32_t h = s_head, t = s_tail;
    return (h - t) & RING_MASK;
}

/* ---------- audio_in_task: 生产者 ---------- */

static void mic_task(void *arg)
{
    static int32_t raw[MIC_DMA_FRAMES];
    static int16_t chunk[MIC_DMA_FRAMES];
    size_t bytes = 0;

    while (1) {
        if (!s_enabled) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (i2s_channel_read(s_rx, raw, sizeof(raw), &bytes, 200) != ESP_OK ||
            bytes < sizeof(int32_t)) {
            continue;                       /* 超时/错误: 下轮重试 */
        }
        uint32_t n = bytes / sizeof(int32_t);
        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = (int16_t)(raw[i] >> 16);  /* 高 16 位, 算术移位保符号 */
        }

        /* 满 → 丢最旧 (锁内前移 tail, 与消费者 pop 互斥) */
        taskENTER_CRITICAL(&s_mux);
        uint32_t free_n = MIC_RING_FRAMES - 1 - ring_used();
        if (n > free_n) {
            uint32_t drop = n - free_n;
            s_tail = (s_tail + drop) & RING_MASK;
            s_overflows += drop;
            if (s_overflows - s_of_log_mark >= MIC_SAMPLE_RATE) {
                s_of_log_mark = s_overflows;
                ESP_LOGW(TAG, "ring overflow, 累计丢 %lu 帧 (消费者太慢)",
                         (unsigned long)s_overflows);
            }
        }
        taskEXIT_CRITICAL(&s_mux);

        uint32_t h = s_head;
        for (uint32_t i = 0; i < n; i++) {
            s_ring[(h + i) & RING_MASK] = chunk[i];
        }
        s_head = (h + n) & RING_MASK;       /* 数据就位后再发布 head */
    }
}

/* ---------- 消费者 ---------- */

esp_err_t bsp_audio_in_read(int16_t *samples, size_t n, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t left = n;
    while (left) {
        taskENTER_CRITICAL(&s_mux);
        uint32_t avail = ring_used();
        uint32_t take = (avail < left) ? avail : (uint32_t)left;
        if (take) {
            uint32_t t = s_tail;
            for (uint32_t i = 0; i < take; i++) {
                samples[i] = s_ring[(t + i) & RING_MASK];
            }
            s_tail = (t + take) & RING_MASK;
        }
        taskEXIT_CRITICAL(&s_mux);
        samples += take;
        left -= take;
        if (left == 0) {
            break;
        }
        if (esp_timer_get_time() > deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

uint32_t bsp_audio_in_overflows(void)
{
    return s_overflows;
}

/* ---------- 开流 / 停流 ---------- */

esp_err_t bsp_audio_in_start(void)
{
    if (!s_enabled) {
        s_head = s_tail = 0;                /* 丢弃上次会话残留 */
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable");
        s_enabled = true;
    }
    return ESP_OK;
}

esp_err_t bsp_audio_in_stop(void)
{
    if (s_enabled) {
        s_enabled = false;
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_rx), TAG, "disable");
    }
    return ESP_OK;
}

/* ---------- 初始化 ---------- */

esp_err_t bsp_audio_in_init(void)
{
    if (s_rx != NULL) {
        return ESP_OK;                      /* 幂等 */
    }

    /* I2S0 RX: 16kHz / 32bit slot / Philips, mono 左槽 (L/R=GND) */
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = MIC_DMA_DESC;
    chan.dma_frame_num = MIC_DMA_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, NULL, &s_rx), TAG, "chan");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,        /* INMP441 无 MCLK 引脚 */
            .bclk = MIC_BCLK_PIN,
            .ws = MIC_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DIN_PIN,
            .invert_flags = { 0, 0, 0 },
        },
    };
    std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  /* IDF6.1 mono 宏默认即 LEFT, 显式写出自文档 (L/R=GND 对应左槽) */
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std), TAG, "std init");
    /* 不在此 enable: start 前不产时钟, INMP441 保持低功耗 */

    BaseType_t ok = xTaskCreatePinnedToCore(mic_task, "audio_in", 4096,
                                            NULL, 20, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");

    ESP_LOGI(TAG, "INMP441 ready (I2S0 RX %dk/32bit slot/mono-left, BCLK=%d WS=%d DIN=%d)",
             MIC_SAMPLE_RATE / 1000, MIC_BCLK_PIN, MIC_WS_PIN, MIC_DIN_PIN);
    return ESP_OK;
}

/* ---------- 电平自检 (S6-A 硬件验收) ---------- */

esp_err_t bsp_audio_in_selftest(uint32_t duration_ms, int32_t *rms, int16_t *peak)
{
    bsp_audio_in_start();

    /* 丢前 100ms: enable 后 DMA 首块可能含上电残留 */
    int16_t dump[240];
    for (uint32_t left = MIC_SAMPLE_RATE / 10; left;) {
        uint32_t take = (left < 240) ? left : 240;
        if (bsp_audio_in_read(dump, take, 500) != ESP_OK) break;
        left -= take;
    }

    int64_t sum2 = 0;
    int32_t count = 0;
    int16_t pk = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        int16_t buf[240];
        if (bsp_audio_in_read(buf, 240, 100) != ESP_OK) continue;
        for (int i = 0; i < 240; i++) {
            int32_t v = buf[i];
            sum2 += (int64_t)v * v;
            int32_t a = (v >= 0) ? v : -v;
            if (a > pk) pk = (int16_t)a;
        }
        count += 240;
    }
    bsp_audio_in_stop();

    if (count == 0) {
        ESP_LOGE(TAG, "[selftest] 没采到数据 — 查接线 BCLK/WS/DIN 与 L/R 接地");
        return ESP_ERR_INVALID_STATE;
    }
    int32_t r = (int32_t)sqrtf((float)sum2 / (float)count);
    ESP_LOGI(TAG, "[selftest] %lu ms: RMS=%ld peak=%d (静默 RMS 应 < 数百, 说话应 > 数千)",
             (unsigned long)duration_ms, (long)r, pk);
    if (rms) *rms = r;
    if (peak) *peak = pk;
    return ESP_OK;
}
