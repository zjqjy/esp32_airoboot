/* S3-C: WS2812 状态灯公共模块 (从 S1 main.c 收编, main/cat_player 共用) */
#ifndef BSP_LED_STATUS_H
#define BSP_LED_STATUS_H

#include <stdint.h>

void led_status_init(void);
void led_status_set(uint8_t r, uint8_t g, uint8_t b);   /* 阻塞发送, 内部乘全局亮度 */

#endif
