/*
 * bsp_audio_in.h — INMP441 麦克风采集驱动 (S6, I2S0 RX)
 * 架构: 事件驱动 + 环形缓冲 (discussion/20261001-mic-in 方案 B, 镜像 S5 输出侧)
 *   生产者 = audio_in_task (I2S DMA 32bit → 转 int16 入环形缓冲)
 *   消费者 = 应用层 bsp_audio_in_read
 *   缓冲满 → 丢最旧 (采集语义: 消费者跟不上时保最新语音), 溢出有计数
 * INMP441 无关断脚: stop = 停 I2S 时钟, 芯片自动进低功耗 (静态 1.4mA 仅在时钟运行时)
 */
#ifndef BSP_AUDIO_IN_H
#define BSP_AUDIO_IN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIC_SAMPLE_RATE     16000   /* 语音链路标准采样率 */
#define MIC_DMA_FRAMES      240     /* 每描述符帧数: 16kHz 下 15ms/块 */
#define MIC_DMA_DESC        6       /* 6 块 = 90ms 硬件缓冲 */
#define MIC_RING_FRAMES     4096    /* 环形缓冲容量 (帧), 2 的幂 = 256ms */

/* 初始化 I2S0 + 启动 audio_in_task. 完成后采集未开流, 采集用 start. */
esp_err_t bsp_audio_in_init(void);

/* 开流: enable I2S 时钟 (INMP441 等到时钟才开始吐数), 清空旧缓冲. */
esp_err_t bsp_audio_in_start(void);

/* 停流: disable I2S 时钟 → INMP441 进低功耗. 重新采集用 bsp_audio_in_start. */
esp_err_t bsp_audio_in_stop(void);

/* 从环形缓冲取 n 帧单声道 int16 采样. 缓冲不够时等数据, 超时返回
 * ESP_ERR_TIMEOUT (已取到的前缀有效, 建议整块调用避免半读). */
esp_err_t bsp_audio_in_read(int16_t *samples, size_t n, uint32_t timeout_ms);

/* 累计溢出丢帧数 (消费者太慢的量化指标, S6-B 验收项). */
uint32_t bsp_audio_in_overflows(void);

/* 电平自检 (S6-A 硬件验收): 采 duration_ms 算 RMS/峰值并打日志,
 * 丢弃前 100ms 上电残留. rms/peak 可传 NULL. 阻塞. 自动 start/stop. */
esp_err_t bsp_audio_in_selftest(uint32_t duration_ms, int32_t *rms, int16_t *peak);

#ifdef __cplusplus
}
#endif
#endif /* BSP_AUDIO_IN_H */
