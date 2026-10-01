/*
 * mic_pins.h — S6 引脚宏 (占位值, 实物接线后登记 BSP_PLAN 引脚表并更新此处)
 * 候选依据: 避开已占 1~7/10/18/21/48、USB 19/20、strap 0/45/46、Flash/PSRAM 26~37、
 *           S5 喇叭 12~15
 * INMP441: L/R 直接接 GND (左声道, 不占 GPIO); 无关断脚, 省电靠停时钟.
 */
#ifndef BSP_MIC_PINS_H
#define BSP_MIC_PINS_H

#define MIC_BCLK_PIN  9
#define MIC_WS_PIN    8
#define MIC_DIN_PIN   11

#endif
