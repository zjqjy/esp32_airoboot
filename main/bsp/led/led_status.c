/*
 * WS2812 状态灯实现 (S1 → S3-C 收编)
 * 引脚 GPIO48, 5V 供电, 3.3V 电平临界但实测可跑 (决策 2026-09-13)
 */
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#include "led_status.h"

#define LED_GPIO 48
#define LED_BRIGHTNESS 30                       /* S1 结论: 30/255 不刺眼 */

static rmt_channel_handle_t chan;
static rmt_encoder_handle_t encoder;

void led_status_init(void)
{
    rmt_tx_channel_config_t cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&cfg, &chan));
    led_strip_encoder_config_t enc = { .resolution = cfg.resolution_hz };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&enc, &encoder));
    ESP_ERROR_CHECK(rmt_enable(chan));
}

void led_status_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t px[3] = {                           /* GRB 顺序 + 全局亮度 */
        g * LED_BRIGHTNESS / 255,
        r * LED_BRIGHTNESS / 255,
        b * LED_BRIGHTNESS / 255,
    };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(chan, encoder, px, 3, &tx));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(chan, portMAX_DELAY));
}
