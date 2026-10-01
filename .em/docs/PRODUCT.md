# 产品定位 — AI 编程搭子 (Code Buddy)

> v1.0 (2026-09-14) · 定位语：给你的 AI Agent 一个会说话的身体

## 1. 市场调研结论（2026-09 GitHub）

| 项目 | 状态 | 与我们的关系 |
|------|------|-------------|
| [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) | 通用语音陪伴，MIT，生态最大（xiaozhi.me + 自部署 server） | **技术底座可复用**：esp-iot-solution 有现成双向流式对话组件，xiaozhi-esp32-server 可自部署 |
| [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) | Anthropic 官方！BLE 硬件：物理按钮批准工具请求 + 会话状态 + 桌宠 | **官方下场验证赛道**；但它只绑定 Claude Desktop，无语音、无国产 agent |
| [Seeed-Solution/vibe-pet](https://github.com/Seeed-Solution/vibe-pet) | Seeed 官方，桌面屏幕宠物显示 Codex/Cursor/Windsurf 状态 | **最近的竞品**；只看不说、绑定 Seeed 硬件 |
| Claude Code Tamagotchi TUI / agent-monitor (menu bar) | Reddit 爆帖、agent-monitor topic 活跃 | 证明"盯 agent"是真痛点，但全是**纯软件** |

**结论**：屏幕显示 agent 状态已被验证，但「**会说话、有灯效、有情绪反馈的实体搭子**」+ 自定义硬件 + 多 agent 是空位。

## 2. 核心差异化（有趣 + 有意义）

**有意义（真痛点）**：agent 跑长任务时人都切去干别的了，回来才发现它 20 分钟前就在等你批准权限。
机器人 = agent 的"传声筒"：该你出手时它喊你，其他时间不烦你。

**有趣（情绪价值）**：agent 状态拟人化成宠物情绪 —
- working → 眼睛左右瞟 + 敲键盘音效 + 灯绿色呼吸
- 等你批准 → 焦急表情 + 语音："Claude 卡住了，等你批准权限！"
- 报错 → 哭脸 + 灯红 + 语音吐槽（台词库可换，社区贡献台词包）
- 完成 → 开心 + 语音播报："搞定！这次改了 3 个文件"
- 养成系统：完成任务得 XP，宠物升级；长时间摸鱼会被碎碎念
- 物理交互：摸屏幕=摸头（有 purr 音效）、按键=让 agent 继续下一任务

## 3. Agent 对接技术方案（PC 端 bridge）

| Agent | 事件机制 | 接法 |
|-------|---------|------|
| Claude Code | Hooks（30 个生命周期事件）| `Stop`/`Notification`/`SessionStart`/`PostToolUse` hook 执行 curl → bridge |
| Codex CLI | `~/.codex/config.toml` 的 `notify` | 配置程序收到 JSON（`agent-turn-complete`；批准事件见 issue #3247 待补）|
| Gemini CLI | 开源 TS，事件可改包/读日志 | adapter 轮询或 patch |
| 通用兜底 | tmux 终端日志监听 | 任意 agent 可接 |

**链路**：`Agent hooks → PC bridge (Python, pip 安装) → MQTT/WS → ESP32 → 表情+语音+灯`。语音播报起步用预合成 MP3（不依赖云端 ASR/TTS），后续再接 xiaozhi 式流式对话。

## 4. 开源 + 自媒体策略

- **三仓一体**：firmware（ESP-IDF）+ bridge（PyPI 包）+ 台词包/表情包（社区 UGC，最易涨 star）
- **传播点**：每个功能 = 一条视频素材（"我把 Claude Code 做成了桌面宠物""它在我编译失败时骂了我"）
- **生态借力**：voice 对话层兼容 xiaozhi 协议 → 蹭其 40k+ star 生态流量；bridge 支持他家 agent → 覆盖 Codex/Cursor 用户
- **硬件开源**：原理图/PCB 一起开源（立创 EDA），别人能复刻 = 涨 star 关键

## 5. 路线调整

S4 由"云端语音对话"改为"bridge + agent 事件播报"（差异化核心，无需云端依赖即可演示）；云端语音对话后移至 S5。详见 ARCHITECTURE.md §7。
