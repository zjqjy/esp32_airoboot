/*
 * S1: WS2812B 红绿蓝颜色循环 500ms 切换（可调亮度）
 * 硬件: GPIO48 → WS2812B (U11)
 * 工具链: ESP-IDF 6.1 + RMT TX + led_strip_encoder (官方 example 模式)
 *
 * 行为: 红(500ms) → 绿(500ms) → 蓝(500ms) → 循环
 * 亮度: BRIGHTNESS 全局系数 (0=灭, 255=全亮)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"

static const char *TAG = "ws2812b";

#define RMT_LED_STRIP_RESOLUTION_HZ  10000000   // 10MHz, 1 tick = 0.1us
#define RMT_LED_STRIP_GPIO_NUM       48         // 原理图 U11 DIN
#define EXAMPLE_LED_NUMBERS          1          // 1 颗 WS2812B
#define COLOR_HOLD_MS                500        // 每色保持 500ms
#define BRIGHTNESS                   30         // 0~255, 30 ≈ 12% 亮度（不刺眼）

// 红绿蓝循环（GRB 顺序填入 buffer，存的是 100% RGB）
typedef struct {
    const char *name;
    uint8_t r, g, b;       // 0~255 全亮度值
} color_t;

static const color_t COLORS[] = {
    {"RED",   255, 0,   0  },
    {"GREEN", 0,   255, 0  },
    {"BLUE",  0,   0,   255},
};
#define COLOR_COUNT (sizeof(COLORS) / sizeof(COLORS[0]))

static uint8_t led_strip_pixels[EXAMPLE_LED_NUMBERS * 3];

static rmt_channel_handle_t led_chan    = NULL;
static rmt_encoder_handle_t led_encoder = NULL;

static void color_cycle_task(void *arg)
{
    uint32_t idx = 0;
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    while (1) {
        // 当前颜色
        const color_t *c = &COLORS[idx];

        // 应用亮度系数：final = original * BRIGHTNESS / 255
        uint8_t g_dim = (uint8_t)((uint16_t)c->g * BRIGHTNESS / 255);
        uint8_t r_dim = (uint8_t)((uint16_t)c->r * BRIGHTNESS / 255);
        uint8_t b_dim = (uint8_t)((uint16_t)c->b * BRIGHTNESS / 255);

        // GRB 顺序填入 buffer
        led_strip_pixels[0] = g_dim;
        led_strip_pixels[1] = r_dim;
        led_strip_pixels[2] = b_dim;

        // 发送并等待完成
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder,
                                     led_strip_pixels,
                                     sizeof(led_strip_pixels),
                                     &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));

        ESP_LOGI(TAG, "LED %s (brightness=%d/%d)", c->name, BRIGHTNESS, 255);

        idx = (idx + 1) % COLOR_COUNT;
        vTaskDelay(pdMS_TO_TICKS(COLOR_HOLD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "WS2812B RGB cycle init on GPIO%d, brightness=%d/%d",
             RMT_LED_STRIP_GPIO_NUM, BRIGHTNESS, 255);

    // 1. 创建 RMT TX 通道
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = RMT_LED_STRIP_GPIO_NUM,
        .mem_block_symbols = 64,
        .resolution_hz     = RMT_LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    // 2. 安装 LED strip encoder
    led_strip_encoder_config_t encoder_config = {
        .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

    // 3. 启用 RMT TX 通道
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    // 4. 启动颜色循环任务
    xTaskCreate(color_cycle_task, "color_cycle", 2048, NULL, 5, NULL);
}
