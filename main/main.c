/*
 * S4: LVGL + esp_lcd 显示栈 — 哭泣猫跳舞
 * 链路: esp_lcd(panel_io_spi+st7789) → esp_lvgl_port(渲染任务+flush) →
 *       cat_player_lvgl(帧推进/舞步/触摸交互/泪滴彩蛋)
 * 交互: 单击切舞步 / 双击切猫 / 连点猫头趴地哭彩蛋 (S3-C 语义原样)
 * 复用: S1 WS2812 (bsp/led) + S2-C CST816 触摸 (bsp/lcd/touch.c)
 * 历史版本: S2/S3 自研渲染栈见 git 历史 (a0cecf1, f63dc8b)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "lcd_esp.h"
#include "touch.h"
#include "led_status.h"
#include "cat_player_lvgl.h"
#include "bsp/audio/bsp_audio_out.h"

static const char *TAG = "s4_lvgl";

#ifdef CONFIG_S5_AUDIO_SELFTEST
/* S5 自检: 440Hz 2s (S5-A 验收) → t1/t2/t3.wav (S5-C/D 验收).
 * 独立任务跑, 不碰 LVGL. 正式发布可在 menuconfig 关闭. */
static void audio_selftest(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));        /* 让显示先稳 */
    bsp_assets_mount();
    ESP_ERROR_CHECK(bsp_audio_out_init());

    ESP_LOGI(TAG, "[S5-A] 440Hz 正弦 2s — 调音器对音准");
    ESP_ERROR_CHECK(bsp_audio_out_play_sine(2000));

    const char *lines[3] = { "/assets/t1.wav", "/assets/t2.wav", "/assets/t3.wav" };
    for (int i = 0; i < 3; i++) {
        ESP_LOGI(TAG, "[S5-C/D] 播 %s", lines[i]);
        if (bsp_audio_out_play_wav(lines[i]) != ESP_OK) {
            ESP_LOGW(TAG, "%s 播放失败 (镜像未烧或参数不符), 跳过", lines[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    ESP_LOGI(TAG, "[S5] 自检完成, 静音待命");
    bsp_audio_out_mute(true);
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    led_status_init();
    ESP_LOGI(TAG, "=== S4: LVGL + esp_lcd 哭泣猫跳舞 ===");

    ESP_ERROR_CHECK(lcd_esp_init());
    led_status_set(0, 255, 0);                  /* 绿 = LCD OK */
    ESP_ERROR_CHECK(touch_init());

    /* LVGL 移植层: 任务+tick 由 esp_lvgl_port 托管 */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    /* 显示注册: 240x40 双缓冲 (~38KB 内部 DMA RAM), 部分刷新 */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_esp_io(),
        .panel_handle = lcd_esp_panel(),
        .buffer_size = 240 * 40,
        .double_buffer = true,
        .hres = 240,
        .vres = 280,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,         /* 屏端按大端收, LVGL 小端缓冲 flush 时交换
                                            (S4 上板实测 RAMCTRL 小端位无效, 见 lcd_esp.c) */
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp 失败");
        return;
    }

    cat_player_lvgl_start();                    /* 注册控件与定时器后返回 */
    /* 此后 LVGL 任务接管渲染, app_main 返回无害 */

#ifdef CONFIG_S5_AUDIO_SELFTEST
    xTaskCreatePinnedToCore(audio_selftest, "s5_test", 6144, NULL, 5, NULL, 0);
#endif
}
