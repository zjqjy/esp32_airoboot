# AI 陪伴机器人 — ESP32-S3 系统架构设计

> 版本: v1.0 (2026-09-14) · 硬件: ESP32-S3-N16R8 · ESP-IDF 6.1

## 1. 总体架构

双入口设计：
1. **Agent 事件流**（差异化核心）：PC 端 agent hooks → bridge → MQTT/WS → 表情+语音+灯效
2. **语音对话流**：语音进 → 云端 ASR/LLM/TTS → 语音出

```
┌───────────────────────────── 应用层 ─────────────────────────────┐
│  app_main → 状态机: IDLE/LISTENING/THINKING/SPEAKING/AGENT_*    │
├────────────┬─────────────┬──────────────┬────────────────────────┤
│ ui/        │ audio/      │ ai/          │ net/                   │
│ LVGL 表情  │ 采集·播放    │ 对话管理      │ WiFi·OTA·WS协议        │
│ 触摸交互   │ AFE·VAD     │ ASR/LLM/TTS  │                        │
├────────────┴─────────────┴──────────────┴────────────────────────┤
│ bsp/ 板级: i2c · i2s_in · i2s_out · lcd · touch · led · button   │
├───────────────────────────────────────────────────────────────────┤
│ 硬件: INMP441 → I2S0    MAX98357 → I2S1    LCD → SPI2    CTP→I2C │
└───────────────────────────────────────────────────────────────────┘
```

## 2. 目录结构（规划）

```
main/
├── main.c              # app_main，组件初始化 + 状态机启动
├── app_state.c/.h      # 应用状态机（IDLE/LISTENING/THINKING/SPEAKING）
├── bsp/                # 板级支持包，每个外设一个 .c，对外只暴露 init + 数据接口
│   ├── bsp_i2c.c       # I2C 总线（触摸用）
│   ├── bsp_audio_in.c  # I2S0 + INMP441 采集
│   ├── bsp_audio_out.c # I2S1 + MAX98357 播放
│   ├── bsp_lcd.c       # SPI + ST7789(待确认) + LVGL 移植层
│   ├── bsp_touch.c     # CTP 触摸驱动
│   └── bsp_led.c       # WS2812B（从 S1 迁入）
├── audio/              # 音频处理链
│   ├── audio_service.c # 音频主循环任务
│   ├── afe_port.c      # esp-sr AFE（AEC/NS/VAD，可选）
│   └── codec/          # Opus 编解码封装
├── ai/                 # 云端 AI 客户端
│   ├── chat_protocol.c # WebSocket/MQTT 流式协议（对齐 xiaozhi 协议可复用现成服务端）
│   └── dialog_mgr.c    # 对话轮次管理
├── net/
│   ├── wifi_mgr.c      # WiFi 连接/重连/配网(AP模式兜底)
│   └── ota.c           # OTA 升级
├── agent/
│   ├── agent_events.c  # 接收 bridge 事件(MQTT/WS JSON)，分发到状态机
│   └── mood_engine.c   # 情绪引擎：agent 状态 → 表情/语音/灯效映射
#
# bridge/ （独立 PyPI 包，PC 端运行）：收集 Claude Code hooks / Codex notify
#   等事件 → 转发 MQTT/WS → 设备。仓库：firmware + bridge + 台词包 三仓一体
└── ui/
    ├── ui_task.c       # LVGL 任务（定时器驱动刷新）
    └── screens/        # 表情页 / 状态页 / 配网页
```

## 3. FreeRTOS 任务与双核分配

| 任务 | 核心 | 优先级 | 栈 | 职责 |
|------|------|--------|-----|------|
| audio_task | 1 | 20(高) | 8KB | I2S DMA 读写、AFE/VAD、Opus 编解码调度 |
| ui_task (LVGL) | 1 | 8 | 16KB | LVGL 渲染 + 触摸轮询 |
| net/protocol_task | 0 | 10 | 8KB | WebSocket 收发、TTS 音频流下发 |
| wifi/事件循环 | 0 | (系统) | - | lwip/WiFi 驱动固定在核 0 |
| app_state_task | 0 | 5 | 4KB | 状态机流转、氛围灯联动 |
| led_task | 0 | 3 | 2KB | WS2812B 呼吸/情绪色 |

原则：**核 0 = 协议栈 + 业务逻辑，核 1 = 实时音频 + 渲染**。音频任务用 `xTaskCreatePinnedToCore` 钉核，DMA 断流是最大风险，优先级必须最高。

## 4. 音频链路设计

```
采集: INMP441 ─I2S0 RX(16kHz/32bit→取高24位)→ 环形缓冲 ─→ [可选 AFE: AEC/NS/VAD]
      ─→ Opus 编码(16kHz mono ~24kbps) ─→ WebSocket 二进制帧 → 云端 ASR

播放: 云端 TTS(Opus/MP3 流) ─→ 解码 16kHz/16bit PCM ─→ 环形缓冲
      ─→ I2S1 TX ─→ MAX98357A ─→ 3718 喇叭(8Ω 2W, GAIN 引脚决定 9/12/15/18dB)
```

- INMP441: L/R 引脚接 GND = 左声道；SCK/WS/SD 三线 I2S（Philips 16-bit slot，标准模式）
- MAX98357: 不需要 MCLK，SD/MODE 引脚电平决定左右声道与输出状态（接 GND=左声道输出）
- 回声消除（AEC）：喇叭播放时麦克风也在收音，全双工对话必须有 AEC，否则 TTS 播放会被误识别 — 这是陪伴机器人和普通语音助手的核心差异
- 起步策略：S3 阶段先做 **按键/触摸说话**（半双工），S5 再上 esp-sr AFE 全双工 + 唤醒词，降低初期难度

## 5. 内存规划（8MB PSRAM + 512KB SRAM）

| 区域 | 用途 |
|------|------|
| PSRAM (Octal 80MHz) | LVGL 帧缓冲 240×280×2 ≈ 134KB×2、Opus/AFE 工作区 (~100KB)、音频环形缓冲、表情图片资源 |
| 内部 SRAM | DMA 描述符、任务栈、lwip、热路径代码（音频解码循环） |
| 内部 SRAM(代码) | WiFi/Opus 热函数用 `ESP_IRAM_ATTR` 防 cache miss 抖动 |

sdkconfig 必开项：`CONFIG_SPIRAM=y`（OPI PSRAM）、`CONFIG_SPIRAM_SPEED_80M`、`CONFIG_ESP32S3_DATA_CACHE_64KB`、LVGL `LV_MEM_CUSTOM`→ps_malloc。

## 6. Flash 分区表（16MB）

```csv
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
otadata,  data, ota,     0x10000,  0x2000    # 预留，未来切双 OTA 时不用重排
factory,  app,  factory, 0x20000,  0x500000  # 5MB 单 app
model,    data, spiffs,  0x520000, 0x300000  # 3MB 预留唤醒词模型(esp-sr)
assets,   data, spiffs,  0x820000, 0x7E0000  # ~7.9MB 表情/字体/语音包
```

> 决策（2026-09-14）：暂用**单 factory 分区**。双 OTA 只在有存量设备要远程升级时才有意义，当前无用户、开发靠 USB 直刷；省出的 5MB 给语音包（assets 2.9MB→7.9MB）。开源发布后若做远程升级，再切双 OTA（届时发版说明要求用户 USB 重刷一次）。

## 7. 实施路线（衔接 .em 步骤流）

| 步骤 | 内容 | 验收标准 |
|------|------|----------|
| S1 ✅ | WS2812B 点灯 | 颜色循环 OK |
| S2 | LCD + 触摸 bring-up | LVGL 显示 + 触摸坐标打印 |
| S3 | 音频双 I2S bring-up | mic 录音回放喇叭（按键触发，半双工）|
| S4 | **bridge + agent 事件播报**（预合成 MP3，无云端依赖）| Claude Code 完成任务→表情+语音+灯效 |
| S5 | 云端语音对话(xiaozhi 协议) + 状态机整合 | 说话→云端→喇叭出声，流式 |
| S6 | 唤醒词(esp-sr) + AEC 全双工 + OTA | 免按键唤醒，远程升级 |

每步独立可验收，BSP 层保证后续替换驱动（如 ST7789→其他 IC）不动上层。

## 8. 风险与待确认

1. **LCD/触摸驱动 IC 未确认** — 拿到原理图后定 ST7789/GC9A01 与 CST816/FT 系列
2. **I2S/SPI/I2C 引脚未分配** — 需原理图；注意 LCD SPI 用 GPSPI2，避开 Flash 所用总线
3. **3.3V 驱动 WS2812B 电平临界** — 已跑通，量产建议 74HCT245
4. **喇叭 2W vs MAX98357 3.2W** — 功率匹配 OK，注意 GAIN 引脚别接 18dB 削波
5. **云端协议选型** — 建议对齐 xiaozhi-esp32 的 WebSocket 协议，服务端生态现成（开源服务器可自部署）
