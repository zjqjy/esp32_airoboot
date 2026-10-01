/*
 * esp_lcd 面板层 (S4-A) — ST7789 240x280, 替换 S2-A 裸驱动
 * 引脚/时序与 S2-A 完全一致 (lcd_init.h 是权威来源), 差异只在驱动层:
 *  - 总线+设备: esp_lcd 组件托管 (panel_io_spi, 内部 DMA + DC 自动拉)
 *  - 字节序: RAMCTRL 小端位被屏忽略 (S4 实测) → esp_lvgl_port swap_bytes 软件交换
 *  - 显存偏移: esp_lcd_panel_set_gap(0,20) → draw_bitmap 自动加偏移
 *    (S2-A 是软件在 RASET 里 y+20, esp_lcd 在窗口计算里等效加)
 */
#ifndef BSP_LCD_ESP_H
#define BSP_LCD_ESP_H

#include <stdint.h>
#include <stdbool.h>
#include "hal/spi_types.h"
#include "esp_lcd_types.h"

/* ---- 引脚分配 (同 S2-A hardware.md 结论) ---- */
#define LCD_ESP_SCK_PIN    2
#define LCD_ESP_MOSI_PIN   3
#define LCD_ESP_CS_PIN     7
#define LCD_ESP_DC_PIN     18
#define LCD_ESP_RES_PIN    21
#define LCD_ESP_BLK_PIN    1

#define LCD_ESP_SPI_HOST   SPI2_HOST
#define LCD_ESP_SPI_MHZ    40       /* S3-C 实测 26MHz 仅 7fps → 提 40; 花屏回退 26 */

/* ---- 分辨率 (S2-A 定论: 玻璃 240x280, 可视区 = 显存[20..299]) ---- */
#define LCD_ESP_W          240
#define LCD_ESP_H          280
#define LCD_ESP_Y_GAP      20       /* set_gap 注入, 勿再软件加 */

esp_err_t lcd_esp_init(void);              /* 总线+panel_io+st7789+厂商时序+开屏背光 */
esp_lcd_panel_io_handle_t lcd_esp_io(void);     /* 传给 lvgl_port_add_disp */
esp_lcd_panel_handle_t lcd_esp_panel(void);
void lcd_esp_backlight(bool on);

#endif
