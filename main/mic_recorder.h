/*
 * mic_recorder.h — S6-C 触摸录音机 (单击屏幕录音 / 再击播放)
 */
#ifndef MIC_RECORDER_H
#define MIC_RECORDER_H

/* 创建全屏录音机界面 + worker 任务后返回 (非阻塞).
 * 须在 lvgl_port_init 之后调用; 音频 init 内部补齐 (幂等).
 * 调用后猫播放器定时器被暂停, 屏幕被录音机界面接管, 重启退出. */
void mic_recorder_start(void);

#endif
