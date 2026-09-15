/*
 * S3-B/C: 哭泣猫跳舞 (自媒体素材)
 * 复用: S1 WS2812 (bsp/led 收编) + S2-A/C LCD/触摸驱动
 * 交互: 单击切舞步 / 双击切猫 / 连点猫头触发趴地哭+泪滴彩蛋 (S3-C)
 * 历史版本: S2-A 点屏验收 / S2-C 触摸涂鸦见 git 历史 (f63dc8b)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lcd_init.h"
#include "led_status.h"
#include "cat_player.h"

static const char *TAG = "s3b_cat";

void app_main(void)
{
    led_status_init();
    ESP_LOGI(TAG, "=== S3-C: 哭泣猫跳舞+互动彩蛋 ===");

    LCD_Init();
    led_status_set(0, 255, 0);                  /* 绿 = LCD OK */

    cat_player_run();                           /* 不返回 */
}
