# bsp_audio_out — MAX98357A 功放驱动 (S5)

## 链路

```
正弦数组 / WAV文件流 → bsp_audio_out_write → 环形缓冲(4096帧)
    → audio_task(核1,优先级20) → i2s_channel_write(DMA 6×240帧=90ms)
    → I2S1 (BCLK=12 LRCK=13 DIN=14) → MAX98357A → 喇叭 8Ω 2W
SD(=15) 拉低 = 功放关断 0.6µA (静音/省电/停播第一拍)
```

## 数据手册硬约束（.em/learning/MAX98357）

- 停播顺序：**先 SD 拉低 → 再 disable I2S**（BCLK 跑时撤 LRCLK 会输出大直流损喇叭）
- 上电顺序：先起 I2S 时钟 → 延时 250ms → 放行 SD（防爆音）
- GAIN 接 GND = 12dB；LRCK 仅支持 8/16/32/44.1/48/88.2/96kHz

## 接口

| API | 说明 |
|-----|------|
| `bsp_audio_out_init()` | SD 关断态 → I2S1 init/enable → 250ms → 放行 → 起 audio_task |
| `bsp_audio_out_write(buf, n, timeout)` | 推采样进环形缓冲（满则阻塞） |
| `bsp_audio_out_play_sine(ms)` | 440Hz 正弦（S5-A 验收） |
| `bsp_audio_out_play_wav(path)` | 流式播放 /assets/*.wav（16bit PCM mono 16k） |
| `bsp_audio_out_drain(ms)` | 等缓冲排空 |
| `bsp_audio_out_stop()` | drain → SD 低 → disable（`start()` 恢复） |
| `bsp_audio_out_mute(bool)` | SD 电平快速静音 |
| `bsp_assets_mount()` | assets 分区只读挂载（失败仅 WAV 不可用） |

## 上板验证步骤（接线后）

1. 接线：BCLK=12 / LRCK=13 / DIN=14 / SD=15，VIN→5V，GAIN→GND，喇叭 8Ω→OUT±
2. `tools\s4_build.bat flash`（固件含 S5 开机自检：440Hz 2s → t1/t2/t3.wav）
3. `tools\flash_assets.bat`（烧 assets 分区；自检里 WAV 段依赖它）
4. 验收：调音器对 440Hz 音准；三段提示音清晰；`/em verify s5-a` 起

## 台词替换

t1~t3.wav 目前是**占位提示音**（tools/gen_tts_wavs.py 合成）。
正式台词：TTS 合成 16kHz/16bit/mono PCM WAV，同名覆盖 tools/assets_wav/，
`python tools/gen_tts_wavs.py fat` 重新出镜像，`tools\flash_assets.bat` 重烧。
