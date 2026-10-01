/*
 * bsp_audio_out.h — MAX98357A 功放驱动 (S5, I2S1 TX)
 * 架构: 事件驱动 + 环形缓冲 (discussion/20261001-audio-out 方案 B)
 *   生产者 = 播放接口 (正弦数组 / WAV 文件流), 消费者 = audio_task 搬运 DMA
 *   缓冲空 → 输出静音帧 (underrun 不爆音)
 * 停播顺序 (数据手册硬要求): 先 SD 拉低 (断功放) → 再 i2s_channel_disable
 *   (BCLK 运行中撤 LRCLK 会输出大直流损坏喇叭)
 */
#ifndef BSP_AUDIO_OUT_H
#define BSP_AUDIO_OUT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE   16000   /* LRCK 白名单内 (8/16/32/44.1/48/88.2/96k) */
#define AUDIO_DMA_FRAMES    240     /* 每描述符帧数: 16kHz 下 15ms/块 */
#define AUDIO_DMA_DESC      6       /* 6 块 = 90ms 硬件缓冲 */
#define AUDIO_RING_FRAMES   4096    /* 环形缓冲容量 (帧), 2 的幂 */
#define AUDIO_SD_QUIET_MS   250     /* 时钟稳定后再放行 SD, 防上电爆音 */

/* 挂载 assets FAT 分区 (只读) + 初始化 I2S1 + 启动 audio_task.
 * 完成后功放放行 (SD 高), 可直接 write. */
esp_err_t bsp_audio_out_init(void);

/* (stop 之后重新发声): enable I2S + 放行 SD. */
esp_err_t bsp_audio_out_start(void);

/* 推 n 帧单声道 int16 采样进环形缓冲. 缓冲满时阻塞, 最多等 timeout_ms. */
esp_err_t bsp_audio_out_write(const int16_t *samples, size_t n, uint32_t timeout_ms);

/* 等待环形缓冲排空 (播完尾巴), 超时返回 ESP_ERR_TIMEOUT. */
esp_err_t bsp_audio_out_drain(uint32_t timeout_ms);

/* 停播: 排空 → SD 拉低 → disable I2S (顺序不可换). 重新发声用 bsp_audio_out_start. */
esp_err_t bsp_audio_out_stop(void);

/* 静音开关: 只动 SD 电平, 不停时钟 (快速 mute; 长期不用请 stop). */
esp_err_t bsp_audio_out_mute(bool mute);

/* 流式播放 WAV 文件 (assets 分区路径, 如 "/assets/t1.wav").
 * 仅支持 16bit PCM mono; 采样率须等于 AUDIO_SAMPLE_RATE. 阻塞至播完 (含 drain). */
esp_err_t bsp_audio_out_play_wav(const char *path);

/* 播放 440Hz 正弦 duration_ms 毫秒 (S5-A 硬件验收用). 阻塞. */
esp_err_t bsp_audio_out_play_sine(uint32_t duration_ms);

/* 挂载 assets FAT 分区 (只读). 失败不影响其他功能, 仅 WAV 播放不可用. */
esp_err_t bsp_assets_mount(void);

bool bsp_audio_out_busy(void);

#ifdef __cplusplus
}
#endif
#endif /* BSP_AUDIO_OUT_H */
