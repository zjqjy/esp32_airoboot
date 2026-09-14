/*
 * S2-A: LCD 点亮 + 分辨率侦探 (ST7789 裸驱动 · 路线A)
 * 硬件: ESP32-S3-N16R8, 引脚见 bsp/lcd_init.h (hardware.md 对齐结论)
 *
 * 侦探结案: 玻璃 240x280, 可视区=显存[20..299] (参考代码藏了 y+20),
 *           终版 LCD_Y_OFFSET=20 + H280, 填充自动全覆盖
 * 验收流:
 *  [验收] 全屏蓝 2s → 无条无噪点
 *  [常规] 红绿蓝全屏轮切 + 8 色条循环
 *  状态灯: 绿色常亮 = LCD 初始化成功 (S1 的 WS2812 复用)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#include "lcd_init.h"
#include "lcd.h"
#include "touch.h"

static const char *TAG = "s2a_lcd";

/* ---------- WS2812 状态灯 (S1 成果复用, 常亮色) ---------- */
#define LED_GPIO 48
static rmt_channel_handle_t led_chan;
static rmt_encoder_handle_t led_encoder;

static void led_init(void)
{
    rmt_tx_channel_config_t cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&cfg, &led_chan));
    led_strip_encoder_config_t enc = { .resolution = cfg.resolution_hz };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&enc, &led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_chan));
}

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t B = 30;                       /* S1 结论: 30/255 不刺眼 */
    uint8_t px[3] = { g * B / 255, r * B / 255, b * B / 255 };  /* GRB */
    rmt_transmit_config_t tx = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, px, 3, &tx));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
}

void app_main(void)
{
    led_init();
    led_set(0, 0, 0);                           /* 灭, 初始化成功再亮绿 */

    ESP_LOGI(TAG, "=== S2-A: ST7789 bring-up (SCK=%d MOSI=%d CS=%d DC=%d RST=%d BLK=%d) ===",
             LCD_SCK_PIN, LCD_MOSI_PIN, LCD_CS_PIN, LCD_DC_PIN, LCD_RES_PIN, LCD_BLK_PIN);

    LCD_Init();
    led_set(0, 255, 0);                         /* 绿 = LCD OK */

    /* ---- 验收: 偏移20+H280 后所有填充自动覆盖可视区, 无条无噪点 ---- */
    ESP_LOGI(TAG, "[验收] 全屏蓝 2s (现在应铺满无条)");
    uint8_t *buf = LCD_GetLineBuf();
    for (int i = 0; i < LCD_W; i++) { buf[2*i] = 0x00; buf[2*i+1] = 0x1F; }
    LCD_SetWindow(0, 0, LCD_W - 1, LCD_H - 1);
    for (int y = 0; y < LCD_H; y++) LCD_SendPixels((const uint16_t *)buf, LCD_W);
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* ---- S2-C: 触摸涂鸦验收 ----
     * 黑底上手指画点: 坐标随动+屏上留痕 = 触摸/显示坐标系闭环 */
    ESP_ERROR_CHECK(touch_init());

    LCD_Clear(C_BLACK);
    ESP_LOGI(TAG, "=== 触摸涂鸦(INT中断) + 心跳灯: 平时闪绿, 触摸变红 ===");
    uint16_t tx, ty;
    bool blink = false;
    while (1) {
        if (!touch_wait(250)) {              /* 250ms 无触摸 → 心跳闪绿 */
            blink = !blink;
            led_set(0, blink ? 255 : 0, 0);
            continue;
        }
        led_set(255, 0, 0);                  /* 触摸: 红 */
        int n = 0;
        while (touch_read(&tx, &ty)) {       /* 按住连读, ~100Hz 画线 */
            if (tx < LCD_W && ty < LCD_H) {
                LCD_FillRect(tx > 2 ? tx - 3 : 0, ty > 2 ? ty - 3 : 0,
                             tx + 3 < LCD_W ? tx + 3 : LCD_W - 1,
                             ty + 3 < LCD_H ? ty + 3 : LCD_H - 1, C_WHITE);
            }
            if (n % 5 == 0) ESP_LOGI(TAG, "触摸 (%u,%u)", tx, ty);  /* 20Hz 采样打印 */
            n++;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        blink = false;
        led_set(0, 255, 0);                  /* 抬手: 回绿 (下轮开始闪烁) */
    }
}
