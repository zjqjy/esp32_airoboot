# AI 陪伴机器人 (ai_roboot) - 项目指南

## 项目元数据
- **产品**: AI 陪伴机器人（语音对话 + 表情屏 + 氛围灯）
- **芯片**: ESP32-S3-N16R8（双核 LX7 @240MHz，16MB Flash，8MB Octal PSRAM）
- **开发环境**: ESP-IDF 6.1（`D:\.espressif\v6.1\esp-idf`）+ VSCode + clangd
- **目标芯片**: esp32s3（xtensa-esp32s3-elf-gcc 15.2.0）

## 硬件清单
| 模块 | 型号 | 接口 | 备注 |
|------|------|------|------|
| 主控 | ESP32-S3-N16R8 | - | 16MB Flash / 8MB PSRAM |
| 麦克风 | INMP441 | I2S RX | 24-bit MEMS 全向麦 |
| 功放 | MAX98357A | I2S TX | 3.2W D类，驱动下方喇叭 |
| 扬声器 | 3718 8Ω 2W | - | 接 MAX98357 输出 |
| 显示屏 | P169H002-CTP 1.69寸 | SPI | 240×280，驱动 IC 待原理图确认（预计 ST7789）|
| 触摸 | CTP 电容触摸 | I2C | 控制器待确认（预计 CST816）|
| 氛围灯 | WS2812B ×1 | RMT | GPIO48（U11，已验证）|

## 关键引脚定义（已验证）
| 外设 | 引脚 | 来源 |
|------|------|------|
| WS2812B DIN | GPIO48 | 原理图 U11，S1 已点灯 |
| LCD SCK/MOSI/CS | 2 / 3 / 7 | SPI2，S2-A 已点亮 |
| LCD DC/RST/BLK | 18 / 21 / 1 | RST 例程19因USB冲突改21；BLK=丝印PWR |
| 触摸 SDA/SCL/INT/RST | 4 / 5 / 6 / 10 | CST816@0x15，接线已接，S2-C 验驱动 |
| USB D-/D+ | 19 / 20 | 禁占；STRAP: 0/45/46；PSRAM: 26~37 禁用 |

> ST7789 关键结论：玻璃 240x280，可视区=显存[20..299]（参考代码藏 y+20 偏移）；RGB565 需大端发送
> 待验证：I2S0(mic)、I2S1(功放) 引脚 → 补充原理图后登记

## 调试惯例（强制执行）
- 串口监视：`idf.py monitor`，日志 TAG 按模块命名（如 ws2812b / audio_in / lvgl）
- 每个外设先单独 bring-up（最小工程验证），再整合进应用
- ESP_ERROR_CHECK 用于初始化路径；运行时路径用 ESP_RETURN_ON_FALSE

## 历史问题记录（决策日志）
- **[2026-09-13]** ESP-IDF 6.1 移除主仓 `led_strip` 组件 → RMT TX + 官方 led_strip_encoder 模式
- **[2026-09-13]** WS2812B 3.3V 电平临界但已跑通；BRIGHTNESS=30（用户嫌太亮）
- **[2026-09-14]** 硬件定型（mic/功放/喇叭/屏/触摸），完成系统架构设计 → 见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

## 禁忌（红线）
- 不烧录 `SWD_LOCK` 类误配置（历史上 J-Link 解锁过一次）
- 不修改 `sdkconfig` 后不重编译直接烧录
- **[2026-09-14]** 产品定位确定：AI 编程搭子（agent 事件播报+情绪反馈），非通用语音陪伴；调研见 [docs/PRODUCT.md](docs/PRODUCT.md)。路线调整：S4=bridge+事件播报（预合成MP3，无云端依赖），云端语音对话移至 S5
- **[2026-09-14]** Flash 分区改单 factory（砍双 OTA）：无存量设备、开发靠 USB，5MB 让给 assets 语音包；开源发布做远程升级时再切双 OTA
- **[2026-09-14]** MVP 方案定稿（docs/MVP.md）：4 状态闭环（working/waiting/done/idle），砍麦克风+WiFi+触摸+图片资源，USB 串口直连 bridge，语音用预合成 WAV；M1 前置条件=确认 LCD 驱动 IC 和引脚
- **[2026-09-14]** 执行顺序调整：先做全硬件驱动层 D1~D5（docs/BSP_PLAN.md，含 MVP 用不到的触摸/麦克风，自媒体逐期拍摄），后进 MVP 集成；引脚登记表维护在 BSP_PLAN.md
- **[2026-09-14]** S2-A 验收通过（HVR-S2-A-01）：字节序大端发送 + 偏移20 两定论；视频素材已攒够（踩坑实录见 .em/problem-log.md 1~6 条）
