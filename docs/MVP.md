# MVP 方案 — 会说话的编程搭子 v0.1

> v1.0 (2026-09-14) · 目标：一条可传播的演示视频 —— "Claude Code 干完活，桌面机器人喊你回来"

## 1. MVP 定义（一句话）

**Claude Code 的状态变化 → 桌面机器人实时表情 + 语音播报 + 氛围灯。**

核心反馈链路闭环，无云端依赖、无语音输入、无配网。

## 2. 范围

### 做（4 个状态闭环）

| Agent 状态 | 触发 Hook | 机器人反应 |
|-----------|-----------|-----------|
| 干活中 | UserPromptSubmit / PostToolUse | 眼睛左右瞟+嘴动，灯绿色呼吸 |
| 等你批准 | Notification | 焦急脸+气泡"!"，语音："Claude 在等你批准！"，灯黄闪 |
| 完成 | Stop | 开心脸，语音："搞定！去检查吧"，灯绿色常亮渐灭 |
| 空闲 | SessionStart/超时 | 呆萌呼吸+偶尔眨眼，灯微光青色 |

### 不做（后置清单）

| 砍掉 | 理由 | 回归版本 |
|------|------|---------|
| INMP441 麦克风/语音输入 | 核心是"输出反馈"，输入是 S5 | S5 |
| WiFi/MQTT | 设备就在电脑旁，USB 串口最稳，零配网 | v1.1（无线上桌）|
| 云端 ASR/TTS/LLM | 事件播报用预合成 WAV 即可演示 | S5 |
| 触摸交互 | 控制器未确认，非反馈主链路 | v1.1（摸头）|
| 表情图片资源 | 全部 LVGL 矢量画，省掉资源管线 | v1.1（表情包 UGC 时）|
| Codex/Gemini adapter | 先只做 Claude Code，架构留 adapter 接口 | v1.1 |
| 养成系统/XP | 情绪反馈优先 | v1.2 |

## 3. 技术要点

### 3.1 传输：USB 串口 JSON Lines

```
PC bridge                          ESP32-S3 (USB CDC)
{"event":"stop",...}  ────────────→  agent_events 任务逐行解析 → mood_engine
```

- ESP32-S3 原生 USB，`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` 或 esp_tinyusb CDC
- 协议就一种消息：`{"event":"stop|notification|working|session_start","agent":"claude-code","msg":"...","ts":123}`
- 无守护进程：hook 直接调 CLI 写串口，故障点最少

### 3.2 bridge（PC 端，纯 Python 单文件可 pip 装）

```
codebuddy send stop          # 读 stdin 的 hook JSON → 摘要 → 写 COM 口
```

Claude Code 侧配置（~/.claude/settings.json）：

```json
{ "hooks": {
    "Stop":        [{ "hooks": [{ "type": "command", "command": "codebuddy send stop" }]}],
    "Notification":[{ "hooks": [{ "type": "command", "command": "codebuddy send notification" }]}],
    "UserPromptSubmit":[{ "hooks": [{ "type": "command", "command": "codebuddy send working" }]}]
}}
```

### 3.3 语音：预合成 WAV，不引解码器

- assets 分区放 16kHz/16bit/单声道 WAV（起步 5~10 句，每句 5s ≈ 160KB，10 句 1.6MB，7.9MB 分区余量大）
- 播放 = 读文件 → I2S1 DMA → MAX98357，无 MP3/Opus 解码依赖
- 台词换内容只需重新合成 WAV 烧 assets，不动固件

### 3.4 表情：LVGL 矢量绘制

两只眼（圆）+ 嘴（弧线）+ 状态气泡，全部 lv_obj 绘制：
idle=慢眨眼 / working=瞳孔左右移+嘴动 / waiting=瞪大+"!"/ done=弯眼笑
→ 零图片资源，改表情即改代码，社区表情包留到 v1.1 再做资源格式。

## 4. 里程碑（预计 1~1.5 周业余时间）

| 里程碑 | 内容 | 验收 |
|--------|------|------|
| M1 屏幕 | SPI+LCD bring-up（预计 ST7789，需先验证）+ LVGL 跑分 | 屏幕显示闪烁的脸 |
| M2 喇叭 | I2S1+MAX98357 播 WAV | 喇叭出声，音量合适 |
| M3 神经 | USB 串口收事件 + mood_engine 状态机 + LED 联动 | 串口发 JSON → 表情/灯变 |
| M4 闭环 | bridge + hooks 配置 + 台词包 + 整合 | 真实 Claude Code 会话全流程反应，录视频 |

> 执行顺序调整（2026-09-14）：M1/M2 之前先完成全部硬件驱动层（含 MVP 用不到的 INMP441 和触摸），
> 详见 [BSP_PLAN.md](BSP_PLAN.md) D1~D5 —— 自媒体「从零搭建」系列按驱动逐期拍摄，细节讲透后再进 MVP 集成。
> 对应关系：D1→M1，D3→M2，D5→M3 前置；D2/D4 为内容期，不阻塞 MVP 主线。

**M1 前置条件**：确认屏幕驱动 IC（1.69寸 240×280 大概率 ST7789V3，先按此初始化试；触摸 IC 顺带 I2C scan）+ LCD/触摸引脚（需要原理图）。

## 5. 演示视频脚本（MVP 完成即拍）

1. 痛点：切屏摸鱼，Claude 停了 20 分钟没人知道（录终端+机器人灰暗脸）
2. 装上 codebuddy：Notification 一响，机器人焦急喊"Claude 在等你批准！"
3. Stop 触发：机器人开心播报 + 灯效，回头检查成果
4. 结尾：开源地址 + 硬件 BOM，"给你的 Agent 一个会说话的身体"
