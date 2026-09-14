/*
 * CST816 电容触摸驱动 (S2-C) — 硬件 I2C (i2c_master 新 API)
 * 引脚对齐 hardware.md: SDA=4 SCL=5 INT=6 RST=10, 地址 0x15
 * 参考代码用 GPIO 位敲软件 I2C → 这里改用硬件外设 (教学对比点)
 */
#ifndef BSP_TOUCH_H
#define BSP_TOUCH_H

#include <stdint.h>
#include <stdbool.h>

#define TOUCH_SDA_PIN   4
#define TOUCH_SCL_PIN   5
#define TOUCH_INT_PIN   6    /* 本阶段轮询不配中断, S2-D 可换 */
#define TOUCH_RST_PIN   10
#define CST816_ADDR     0x15

/* 寄存器 (与参考代码 cst816.h 一致) */
#define CST816_REG_GESTURE   0x01
#define CST816_REG_FINGER    0x02
#define CST816_REG_XPOSH     0x03
#define CST816_REG_CHIPID    0xA7

esp_err_t touch_init(void);                      /* 总线+复位+读ChipID+INT中断 */
uint8_t   touch_chip_id(void);                   /* CST816S 通常 0xB4 */
bool      touch_wait(uint32_t timeout_ms);       /* 等 INT 中断, true=等到触摸事件 */
int       touch_int_level(void);                 /* 诊断: INT 脚原始电平 */
bool      touch_read(uint16_t *x, uint16_t *y);  /* 读坐标, true=手指按着 */

#endif
