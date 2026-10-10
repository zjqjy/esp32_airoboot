#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "lcd_esp.h"
#include "disp_test.h"

static const char *TAG = "disp_test";

/* 分带直写: 240x40 带 = 19.2KB 内部 RAM; 单带 DMA@40MHz 约 4ms,
 * 每带画完延时再复用缓冲, 避免异步传输未完就被改写 (esp_lcd 颜色传输走队列) */
#define BAND_H        40
#define BAND_N        (LCD_ESP_H / BAND_H)
#define BAND_STEP_MS  60

static uint16_t s_band[LCD_ESP_W * BAND_H];

/* 面板按大端解释像素 (S4 定论: RAMCTRL 小端位被本屏忽略); LVGL 侧由
 * swap_bytes 在 flush 补交换. 自检直写同一物理路径, 需软件预换才能呈现
 * "预期色". 若纯色步颜色对不上, 这就是字节序/色序的定位点. */
static inline uint16_t px_be(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

static void fill_const(uint16_t c)
{
    uint16_t v = px_be(c);
    for (int i = 0; i < LCD_ESP_W * BAND_H; i++) {
        s_band[i] = v;
    }
}

static void frame_draw(void)
{
    esp_lcd_panel_handle_t panel = lcd_esp_panel();
    for (int b = 0; b < BAND_N; b++) {
        esp_lcd_panel_draw_bitmap(panel, 0, b * BAND_H, LCD_ESP_W, (b + 1) * BAND_H, s_band);
        vTaskDelay(pdMS_TO_TICKS(BAND_STEP_MS));
    }
}

static void bands_show(const char *what, uint32_t hold_ms)
{
    frame_draw();
    ESP_LOGI(TAG, ">>> 应看到: %s", what);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
}

void disp_test_run(void)
{
    ESP_LOGI(TAG, "==== S6-D 屏幕自检开始 (约 15s), 对照串口逐条看屏 ====");

    lcd_esp_backlight(true);
    ESP_LOGI(TAG, "[1] 背光 GPIO1 已强制拉亮; 若整体仍暗到要凑近才见图 → 查 BL 跳线/背光供电");

    /* 背光明灭 (白底上明灭最易分辨): 无明灭 = BL 线断/接错; 有明灭 = 背光链路 OK */
    fill_const(0xFFFF);
    frame_draw();
    ESP_LOGI(TAG, "[2] 背光明灭 x3:");
    for (int i = 0; i < 3; i++) {
        lcd_esp_backlight(false);
        ESP_LOGI(TAG, "[2]   BL 灭 (屏应变暗)");
        vTaskDelay(pdMS_TO_TICKS(450));
        lcd_esp_backlight(true);
        ESP_LOGI(TAG, "[2]   BL 亮 (屏应变白亮)");
        vTaskDelay(pdMS_TO_TICKS(450));
    }
    ESP_LOGI(TAG, "[2] 判读: 白屏无明灭 = BL 线/供电问题; 有明灭 = 背光链路 OK");

    fill_const(0xF800);
    bands_show("全屏纯红 (若变青 = INVON/色序线索)", 1200);
    fill_const(0x07E0);
    bands_show("全屏纯绿", 1200);
    fill_const(0x001F);
    bands_show("全屏纯蓝", 1200);
    fill_const(0xFFFF);
    bands_show("全屏白 (条纹/花屏在此步最易暴露)", 1200);
    fill_const(0x0000);
    bands_show("全屏黑", 800);

    /* 左暗右亮灰阶: 中途断裂/回折 = 字节序; 一段段跳变 = gamma/带宽线索 */
    for (int x = 0; x < LCD_ESP_W; x++) {
        uint32_t g5 = (uint32_t)x * 31 / (LCD_ESP_W - 1);
        uint32_t g6 = (uint32_t)x * 63 / (LCD_ESP_W - 1);
        uint16_t v = px_be((uint16_t)((g6 << 11) | (g5 << 5) | g5));
        for (int y = 0; y < BAND_H; y++) {
            s_band[y * LCD_ESP_W + x] = v;
        }
    }
    bands_show("灰阶: 左黑右白平滑渐变", 1500);

    /* 8 竖彩条 (经典校验序): 色相全对 = 像素格式 OK */
    static const uint16_t bars[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
    };
    for (int x = 0; x < LCD_ESP_W; x++) {
        uint16_t v = px_be(bars[x / (LCD_ESP_W / 8)]);
        for (int y = 0; y < BAND_H; y++) {
            s_band[y * LCD_ESP_W + x] = v;
        }
    }
    bands_show("彩条: 白/黄/青/绿/品/红/蓝/黑 8 竖条", 1500);

    ESP_LOGI(TAG, "==== 屏幕自检结束, 马上进正常界面; 把异常画面+对应步骤号报回来 ====");
    return;
}
