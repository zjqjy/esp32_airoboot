# BSP 驱动层开发计划 — 「从零搭建桌面搭子」系列

> v1.0 (2026-09-14) · 原则：每个驱动 = 一期视频 + 一个独立可验收的 bsp 模块
> 顺序即内容叙事依赖链：有屏 → 能摸 → 会出声 → 能听 → 集成 MVP

## 引脚登记表（拿原理图后填，永久维护在 CLAUDE.md）

| 外设 | 信号 | GPIO | 状态 |
|------|------|------|------|
| WS2812B | DIN | 48 | ✅ 已验证 |
| LCD | MOSI / SCLK / CS / DC / RST / BL | ? | ⬜ 待原理图 |
| 触摸 | SDA / SCL / INT / RST | ? | ⬜ 待原理图 |
| MAX98357 | BCLK / LRCK / DIN / GAIN / SD | ? | ⬜ 待原理图 |
| INMP441 | SCK / WS / SD / L_R | ? | ⬜ 待原理图 |

## D1. LCD 显示（ST7789 · SPI）— 「点亮第一块屏」

**视频讲解点**：SPI 时序基础 / ST7789 初始化序列在干什么（SLPOUT→COLSET→MADCTL）/
esp_lcd 组件 + LVGL 9 移植分层 / DMA 双缓冲为什么要放 PSRAM、cache 对齐

**实现路径**：
1. 裸 `esp_lcd_panel_io_spi` + `esp_lcd_new_panel_st7789`，刷纯色测试（红绿蓝各 1s → 丝印判断偏色/反色）
2. LVGL 9（component manager 拉取）+ lv_port：`lv_timer` 驱动 + `esp_lcd_panel_draw_bitmap` 回调
3. 双缓冲 240×280×2×2 ≈ 268KB → PSRAM（`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`）

**验收**：LVGL 官方 demo 流畅跑，无明显撕裂
**已知坑**：240×280 是 ST7789 partial area → 花屏/偏移先查 MADCTL 和 Y offset；背光极性；
SPI mode 0/3 选择；PSRAM 缓冲要 `ESP_CACHE_MALIGNED_SIZE` 对齐否则 DMA 撕裂

## D2. 触摸（CTP · I2C）— 「屏幕活过来了」

**视频讲解点**：I2C 总线扫描（侦探环节：0x15=CST816 / 0x38=FT 系列 / 0x5D=GT911）/
电容触摸原理 30 秒版 / LVGL indev 注册 + 坐标映射（触摸分辨率≠屏分辨率）

**实现路径**：
1. `i2c_master` 总线初始化 + scan 打地址 → 定控制器型号
2. 读坐标轮询版跑通 → 换 INT 中断+阈值，降低 I2C 占用
3. LVGL `lv_indev_create` + read_cb 对接

**验收**：LVGL 画个按钮，按下有 pressed 态，坐标打印与手指位置一致
**已知坑**：GT911 复位时序决定 I2C 地址（INT 拉高/低）；坐标 X/Y 镜像要改扫描方向寄存器而非软件翻转（除非懒得改）；INT 引脚必须配上下拉

## D3. 音频输出（MAX98357 · I2S TX）— 「它开口了」

**视频讲解点**：I2S 三根线各是什么（BCLK/LRCK/DATA）/ 为什么 MAX98357 不需要 MCLK /
16bit 标准模式 vs 左对齐 / GAIN 引脚电平=增益档位（9/12/15/18dB）/ SD 引脚=声道选择+关断

**实现路径**：
1. `i2s_std` TX 通道 16kHz/16bit/mono，先播 440Hz 正弦波（数组生成，无文件依赖）
2. WAV 播放器：fopen assets 分区 → 解析 44 字节头 → `i2s_channel_write`
3. 合成 3 句台词 WAV（在线 TTS 或剪映导出）烧进 assets 分区

**验收**：正弦波音调准确（对调音器 app），WAV 台词清晰，音量旋钮级合理（GAIN 选 12dB 起步）
**已知坑**：爆音=采样率不匹配或缓冲欠载（双 buffer+重采样对齐）；嗡嗡声=共地问题；
声音发闷=LRCK 极性反了（左右声道混叠）；2W 喇叭+18dB 增益会削波失真

## D4. 音频输入（INMP441 · I2S RX）— 「它听得见了」

**视频讲解点**：MEMS 麦克风原理 / 24bit 数据在 32bit slot 的高 24 位、符号位扩展 /
L_R 引脚接 GND=左声道 / RX 和 TX 用两个 I2S 控制器（I2S0 收 / I2S1 发）

**实现路径**：
1. `i2s_std` RX 32bit slot 采数 → 高 24 位符号扩展成 int16
2. **内容爆点**：实时波形画到屏幕上（LVGL line chart，D1 成果复用）
3. 录 3 秒 → 存 PSRAM → D3 播放回放（麦克风→喇叭 loopback 闭环）

**验收**：屏幕波形随说话起伏；回放录音清晰
**已知坑**：静音或半音量=L_R 声道选错；波形毛刺=没做直流偏置去除（减去均值）；
INMP441 采样率上限 ~48kHz 但 16k 足够

## D5. 集成收编 — 「硬件层毕业」

各驱动例程收编进 `main/bsp/`（接口规范：`bsp_x_init()` + 数据/事件接口，全局只一个 I2C 总线句柄），
跑通 BSP 冒烟测试（屏+触摸+播一句+录音回放+灯效一键自检）。
此后按 [MVP.md](MVP.md) M3/M4 做 USB 串口事件链路和 bridge。

## 内容线备注

- D1~D4 每期结尾埋下期钩子：D1 结尾"屏幕亮了但它还听不见你"→ D2 …
- 每期素材自然沉淀 = 该驱动的 README 调试记录（翻车截图别删，是内容也是文档）
- S1（WS2812B）可剪成第 0 期（点灯篇）
