/*
 * ST7789 裸驱动 (S2-A) — 移植自 P169H002 例程, 目标 ESP32-S3 + IDF 6.1
 * 引脚对齐: .em/discussion/20260914-lcd-basic/hardware.md
 * 路线: 裸写寄存器(教学) → S2-D 后收编 esp_lcd
 */
#ifndef BSP_LCD_INIT_H
#define BSP_LCD_INIT_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"

/* ---- 引脚分配（hardware.md 硬件对齐结论）---- */
#define LCD_SCK_PIN    2
#define LCD_MOSI_PIN   3    /* 转接板丝印 SDA = SPI 数据, 不是 I2C! */
#define LCD_CS_PIN     7
#define LCD_DC_PIN     18   /* 厂商建议 UART1 用 17/18, 本项目弃用 UART1 */
#define LCD_RES_PIN    21   /* 例程 19 = USB D- 冲突, 已改 21 */
#define LCD_BLK_PIN    1    /* 转接板丝印 PWR = 背光使能 */

#define LCD_SPI_HOST   SPI2_HOST
#define LCD_SPI_MHZ    26   /* 起步保守值, S2-D 做 40/80 对比 */

/* ---- 分辨率终版(S2-A 对照参考代码定论) ----
 * 玻璃 240x280, 可视区 = 显存 [20..299] (上下各藏20行)
 * 铁证: 参考代码 LCD_Address_Set 里 y+20 (例程 LCD_H=284 是它自己的笔误,
 * 多写4行落隐藏区所以看不出) → 偏移20 + H280 即全覆盖 */
#define LCD_W 240
#define LCD_H 280
#define LCD_Y_OFFSET 20

/* ---- RGB565 颜色宏 ----
 * 格式: RRRRR GGGGGG BBBBB (高->低), memcpy 大端发送
 */
#define C_BLACK    0x0000
#define C_NAVY     0x000F
#define C_DGREEN   0x03E0
#define C_DCYAN    0x03EF
#define C_MAROON   0x7800
#define C_PURPLE   0x780F
#define C_OLIVE    0x7BE0
#define C_LGRAY    0xC618
#define C_DGRAY    0x7BEF
#define C_BLUE     0x001F
#define C_GREEN    0x07E0
#define C_CYAN     0x07FF
#define C_RED      0xF800
#define C_MAGENTA  0xF81F
#define C_YELLOW   0xFFE0
#define C_WHITE    0xFFFF
#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

/* ---- API ---- */
void LCD_Init(void);                                    /* 总线+GPIO+初始化序列+开背光 */
void LCD_Backlight(bool on);                            /* 背光开关 */
void LCD_SetWindow(uint16_t x1, uint16_t y1,
                   uint16_t x2, uint16_t y2);           /* 显存窗口(列/行地址) */
void LCD_WriteCmd(uint8_t cmd);                         /* DC=0 发指令 */
void LCD_WriteData8(uint8_t d);                         /* DC=1 发 1 字节参数 */
void LCD_WriteData16(uint16_t d);                       /* DC=1 发 16bit 参数(坐标等) */
void LCD_SendPixels(const uint16_t *px, uint32_t n);    /* DMA 批量发像素 */
uint8_t *LCD_GetLineBuf(void);                          /* 内部行缓冲(字节大端, 绘图层复用) */

#endif
