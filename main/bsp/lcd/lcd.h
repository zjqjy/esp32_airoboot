/* 基础绘图 API (S2-A: 填充矩形 + 色条; S2-B 扩展点/线/圆/字符) */
#ifndef BSP_LCD_H
#define BSP_LCD_H
#include <stdint.h>
#include "lcd_init.h"

void LCD_FillRect(uint16_t x1, uint16_t y1,
                  uint16_t x2, uint16_t y2, uint16_t color);
void LCD_Clear(uint16_t color);                       /* 全屏清屏 */
void LCD_Color_Bar(void);                             /* 8 色横条测试图 */

#endif
