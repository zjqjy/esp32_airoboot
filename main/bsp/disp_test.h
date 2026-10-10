/*
 * disp_test.h — S6-D 屏幕链路自检 (2026-10-04 排障新增)
 * 现象: 上板屏幕"黑底条纹/隐约见图". 在 LVGL 接管前独占面板,
 * 直写 esp_lcd 依次验证: 背光链路 → 纯色 → 灰阶 → 彩条.
 * 串口每步打印"应看到什么", 对照实物判读即可定位坏环节.
 */
#ifndef BSP_DISP_TEST_H
#define BSP_DISP_TEST_H

void disp_test_run(void);   /* 阻塞约 15s, 仅可在 lcd_esp_init 之后 lvgl_port_init 之前调 */

#endif
