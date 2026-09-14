/*
 * 基础绘图实现 — 核心套路: 设窗口 → 逐行填像素
 * LCD_FillRect 一次设窗, 每行发一行像素 (行缓冲复用, 这是裸驱动的经典优化)
 */
#include <string.h>
#include "lcd.h"

void LCD_FillRect(uint16_t x1, uint16_t y1,
                  uint16_t x2, uint16_t y2, uint16_t color)
{
    if (x2 < x1 || y2 < y1) return;
    uint16_t w = x2 - x1 + 1;
    uint16_t h = y2 - y1 + 1;

    LCD_SetWindow(x1, y1, x2, y2);

    /* 行缓冲按大端字节填色, 逐行 DMA 发出 */
    uint8_t *buf = LCD_GetLineBuf();
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (uint16_t i = 0; i < w; i++) {
        buf[2 * i]     = hi;
        buf[2 * i + 1] = lo;
    }
    for (uint16_t r = 0; r < h; r++) {
        LCD_SendPixels((const uint16_t *)buf, w);
    }
}

void LCD_Clear(uint16_t color)
{
    LCD_FillRect(0, 0, LCD_W - 1, LCD_H - 1, color);
}

void LCD_Color_Bar(void)
{
    const uint16_t bars[8] = { C_RED, C_GREEN, C_BLUE, C_YELLOW,
                               C_CYAN, C_MAGENTA, C_WHITE, C_BLACK };
    uint16_t bh = LCD_H / 8;
    for (int i = 0; i < 8; i++) {
        LCD_FillRect(0, i * bh, LCD_W - 1, (i + 1) * bh - 1, bars[i]);
    }
}
