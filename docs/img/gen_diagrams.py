#!/usr/bin/env python3
"""gen_diagrams.py — 生成 cat_player 讲解文档的图解 PNG (docs/img/)
Windows 自带微软雅黑, 无第三方依赖 (仅 Pillow)。
"""
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(__file__))
FONT = "C:/Windows/Fonts/msyh.ttc"

def F(size, bold=False):
    idx = 1 if bold else 0
    return ImageFont.truetype(FONT, size, index=idx)

# 配色 (扁平, 打印友好)
INK    = (55, 60, 70)
SUB    = (120, 126, 138)
BLUE   = (66, 133, 244)
BLUE_L = (232, 241, 255)
ORANGE = (240, 138, 30)
OR_L   = (255, 240, 220)
GREEN  = (52, 148, 90)
GR_L   = (226, 245, 233)
RED    = (222, 84, 74)
RD_L   = (253, 231, 229)
GRAY_L = (243, 244, 246)
YEL_L  = (255, 249, 219)

def canvas(w, h):
    im = Image.new("RGB", (w, h), (255, 255, 255))
    return im, ImageDraw.Draw(im)

def box(d, xy, fill, r=10, outline=None, width=2):
    d.rounded_rectangle(xy, radius=r, fill=fill,
                        outline=outline or (200, 205, 212), width=width)

def text(d, xy, s, size=20, color=INK, bold=False, anchor="la"):
    d.text(xy, s, font=F(size, bold), fill=color, anchor=anchor)

def arrow(d, p1, p2, color=INK, width=3, head=12):
    d.line([p1, p2], fill=color, width=width)
    import math
    ang = math.atan2(p2[1]-p1[1], p2[0]-p1[0])
    for da in (2.6, -2.6):
        d.line([p2, (p2[0]+head*math.cos(ang+da), p2[1]+head*math.sin(ang+da))],
               fill=color, width=width)

# ---------------------------------------------------------------- 01 三层模型
def d01_layers():
    im, d = canvas(900, 640)
    text(d, (30, 24), "屏 = 三层, 你只跟最底层说话", 28, INK, True)
    # ESP32
    box(d, (60, 100, 840, 190), BLUE_L, outline=BLUE)
    text(d, (450, 120), "ESP32-S3  (你的代码)", 22, BLUE, True, "ma")
    text(d, (450, 155), "把颜色编号写进显存 —— 唯一能做的事", 18, SUB, False, "ma")
    arrow(d, (450, 190), (450, 250), INK)
    text(d, (465, 205), "SPI 40MHz: 每秒 4MB 的字节流 (只写不读)", 17, SUB)
    # GRAM
    box(d, (60, 250, 840, 390), YEL_L, outline=(225, 200, 90))
    text(d, (450, 262), "ST7789 控制器里的显存 GRAM  240×320 格", 21, INK, True, "ma")
    # 可视区
    vx0, vy0, vx1, vy1 = 180, 315, 720, 380
    d.rectangle((vx0, vy0, vx1, vy1), fill=(255,255,255), outline=ORANGE, width=3)
    text(d, (450, 322), "玻璃贴在这里: 第 20~299 行 (共 280 行)", 19, ORANGE, True, "ma")
    text(d, (450, 350), "← LCD_Y_OFFSET = 20 的来历 (S2-A 侦探结论)", 16, SUB, True, "ma")
    # 隐藏区标注
    text(d, (450, 294), "↑ 上方 20 行隐藏区", 14, SUB, True, "ma")
    arrow(d, (450, 390), (450, 448), INK)
    text(d, (465, 405), "屏幕每 1/60 秒把显存抄写到玻璃 (自刷新)", 17, SUB)
    # Glass
    box(d, (180, 448, 720, 560), GR_L, outline=GREEN)
    text(d, (450, 468), "玻璃 (你眼睛看的)", 22, GREEN, True, "ma")
    for i in range(6):
        box(d, (215+i*80, 505, 275+i*80, 545), (255, 220, 120), r=4, outline=(230, 190, 90))
    text(d, (450, 585), "你随时写都不会闪: 你写后台黑板, 屏幕以恒定 60Hz 抄到前台", 18, SUB, True, "ma")
    im.save(os.path.join(OUT, "01_layers.png"))

# ---------------------------------------------------------------- 02 打字机
def d02_typewriter():
    im, d = canvas(900, 560)
    text(d, (30, 24), "画画的唯一姿势: 设窗口 → 灌像素 (打字机模型)", 28, INK, True)
    # GRAM grid
    ox, oy, cw = 80, 100, 34
    cols, rows = 12, 8
    # window
    wx0, wy0, wx1, wy1 = ox+3*cw, oy+2*cw, ox+9*cw, oy+6*cw
    d.rectangle((wx0, wy0, wx1, wy1), fill=BLUE_L)
    for r in range(rows+1):
        d.line((ox, oy+r*cw, ox+cols*cw, oy+r*cw), fill=(215, 219, 224))
    for c in range(cols+1):
        d.line((ox+c*cw, oy, ox+c*cw, oy+rows*cw), fill=(215, 219, 224))
    d.rectangle((wx0, wy0, wx1, wy1), outline=BLUE, width=3)
    text(d, ((wx0+wx1)//2, wy0+14), "窗口 LCD_SetWindow(0, y0, 239, y1)", 17, BLUE, True, "ma")
    # fill path
    for r in range(2, 6):
        arrow(d, (wx0+8, oy+r*cw+cw//2), (wx1-8, oy+r*cw+cw//2), ORANGE, 3)
        if r < 5:
            arrow(d, (wx1-14, oy+(r+1)*cw-4), (wx0+14, oy+(r+1)*cw-4), (200,205,212), 2)
    text(d, (wx1+30, oy+3*cw), "0x2A/0x2B/0x2C", 17, INK, True)
    text(d, (wx1+30, oy+3*cw+26), "圈范围", 17, SUB)
    text(d, (wx1+30, oy+4*cw), "0x2C 后只管灌", 17, INK, True)
    text(d, (wx1+30, oy+4*cw+26), "自动逐行回绕", 17, SUB)
    text(d, (ox, oy+rows*cw+40), "灌满一行 → 自动回到行首 → 填下一行 → 到窗口右下角自动停", 20, INK, True)
    text(d, (ox, oy+rows*cw+80), "没有画圆/贴图/清屏命令 —— 一切画面 = 这两步的排列组合", 18, SUB)
    im.save(os.path.join(OUT, "02_typewriter.png"))

# ---------------------------------------------------------------- 03 活动带
def d03_band():
    im, d = canvas(900, 620)
    text(d, (30, 24), "活动带: 每帧只重写『上一帧猫 ∪ 这一帧猫』占过的行", 27, INK, True)
    ox, oy, W, H = 150, 80, 200, 430   # 屏
    d.rectangle((ox, oy, ox+W, oy+H), fill=GRAY_L, outline=INK, width=2)
    text(d, (ox+W//2, oy-30), "屏 240×280", 17, SUB, True, "ma")
    cx = ox + (W-130)//2
    # 上一帧: 橙色虚线感(细框)
    a_y0, a_y1 = oy+20, oy+200
    d.rectangle((cx, a_y0, cx+130, a_y1), outline=ORANGE, width=3)
    # 这一帧: 蓝色实心
    b_y0, b_y1 = oy+44, oy+224
    d.rectangle((cx, b_y0, cx+130, b_y1), fill=BLUE_L, outline=BLUE, width=3)
    text(d, (cx+65, b_y0+12), "这一帧", 15, BLUE, True, "ma")
    # 右侧标注 (引线)
    arrow(d, (cx+130, a_y0+10), (ox+W+14, a_y0+10), ORANGE, 2)
    text(d, (ox+W+20, a_y0-2), "上一帧猫 y∈[20,200)", 16, ORANGE, True)
    arrow(d, (cx+130, b_y0+10), (ox+W+14, b_y0+10), BLUE, 2)
    text(d, (ox+W+20, b_y0-2), "这一帧猫 y∈[44,224)", 16, BLUE, True)
    # 左侧红色活动带括号
    d.line((ox-26, oy+20, ox-26, oy+224), fill=RED, width=4)
    d.line((ox-26, oy+20, ox-12, oy+20), fill=RED, width=4)
    d.line((ox-26, oy+224, ox-12, oy+224), fill=RED, width=4)
    text(d, (ox-34, oy+115), "活动带 [20,224)", 17, RED, True, "rm")
    text(d, (ox+8, oy+H+36), "每帧只组装/推送带子里的行; 带外显存没人动过 → 不用重画, 省约 2/3 传送", 18, INK, True)
    text(d, (ox+8, oy+H+70), "残影从哪来: 旧位置若不重铺底色, 猫走后会留影子 —— 所以并集里包含上一帧", 16, SUB)
    im.save(os.path.join(OUT, "03_band.png"))

# ---------------------------------------------------------------- 04 4bpp
def d04_pack():
    im, d = canvas(900, 560)
    text(d, (30, 24), "4bpp: 两个像素挤一个字节, 查 16 色调色板还原", 28, INK, True)
    # 字节行
    bx, by = 90, 120
    vals = ["3","7","1","15"]
    for i, v in enumerate(vals):
        x = bx + i*95
        d.rectangle((x, by, x+90, by+70), fill=BLUE_L, outline=BLUE, width=2)
        d.line((x+45, by, x+45, by+70), fill=BLUE, width=2)
        text(d, (x+22, by+22), v, 24, BLUE, True, "ma")
        text(d, (x+67, by+22), "7", 24, (160,168,178), True, "ma")
        text(d, (x+22, by+74), "高 4 位", 14, SUB, anchor="ma")
        text(d, (x+67, by+74), "低 4 位", 14, SUB, anchor="ma")
    text(d, (bx+4*95+30, by+22), "…", 30, INK)
    text(d, (bx, by-38), "flash 里的帧数据 (索引编号, 每像素半个字节)", 18, INK, True)
    # arrows to pixels
    arrow(d, (bx+45, by+110), (bx+45, by+170), ORANGE)
    text(d, (bx+60, by+122), "1. 按 i>>1 取字节; 偶数取高、奇数取低", 17, INK)
    # palette
    py = by+170
    for i, c in enumerate([(255,255,255),(120,90,60),(255,220,120),(90,180,235)]):
        d.rectangle((bx+i*95, py, bx+i*95+90, py+46), fill=c, outline=(200,205,212), width=2)
        text(d, (bx+i*95+45, py+52), f"palette[{i*4%16}]", 13, SUB, anchor="ma")
    text(d, (bx+420, py+14), "2. 编号 → 查调色板 → RGB565", 17, INK)
    # big endian
    ey = py+100
    d.rectangle((bx, ey, bx+150, ey+56), fill=OR_L, outline=ORANGE, width=2)
    text(d, (bx+75, ey+10), "H | L", 22, INK, True, "ma")
    text(d, (bx+75, ey+34), "row_buf 里摆好", 14, SUB, anchor="ma")
    arrow(d, (bx+160, ey+28), (bx+260, ey+28), INK)
    d.rectangle((bx+270, ey, bx+430, ey+56), fill=GR_L, outline=GREEN, width=2)
    text(d, (bx+350, ey+10), "ST7789 显存", 18, GREEN, True, "ma")
    text(d, (bx+350, ey+34), "要求高字节先到 (大端)", 13, SUB, anchor="ma")
    text(d, (bx, ey+80), "不摆大端 → 红变蓝 (S2-A 实测坑)", 17, RED, True)
    im.save(os.path.join(OUT, "04_pack4bpp.png"))

# ---------------------------------------------------------------- 05 状态机
def d05_state():
    im, d = canvas(980, 600)
    text(d, (30, 24), "主循环 = 一个 5 段状态机 (彩蛋是临时串场)", 28, INK, True)
    # 平时 (大框)
    box(d, (60, 110, 920, 380), GRAY_L, outline=INK)
    text(d, (90, 126), "平时: 3 只猫轮换跳舞 (双击: 张望→碗装眼泪→双猫哭哭)", 19, INK, True)
    steps = ["① 彩蛋退出?", "② 彩蛋进入?", "③ 切换检测\na≠ANIMS[cur]", "④ 渲染活动带\n+ 蓝灯呼吸", "⑤ GIF 帧推进\n(now≥fi_due)"]
    sx = 90
    for s in steps:
        box(d, (sx, 180, sx+150, 300), (255,255,255), outline=BLUE)
        for i, line in enumerate(s.split("\n")):
            text(d, (sx+75, 205+i*34), line, 16, INK, True, "ma")
        sx += 168
    arrow(d, (240, 240), (256, 240), INK); arrow(d, (408, 240), (424, 240), INK)
    arrow(d, (576, 240), (592, 240), INK); arrow(d, (744, 240), (760, 240), INK)
    arrow(d, (834, 380), (165, 380), (200,205,212), 2)
    text(d, (500, 336), "每圈 ~30ms, 无限循环; 交互轮询 15ms 限频插在圈与圈之间", 15, SUB, True, "ma")
    # egg bubble
    box(d, (240, 440, 740, 560), RD_L, outline=RED)
    text(d, (490, 458), "彩蛋 (连点猫头 ≥3 次触发)", 20, RED, True, "ma")
    text(d, (490, 492), "存现场(猫+舞步) → 切趴地哭+舞步冻结+滋泪滴 28 帧", 17, INK, anchor="ma")
    text(d, (490, 524), "放完自动回读现场, 继续刚才的舞步", 17, INK, anchor="ma")
    arrow(d, (300, 380), (330, 440), RED)
    arrow(d, (650, 440), (680, 380), RED)
    text(d, (330, 405), "触发", 15, RED); text(d, (688, 405), "收工", 15, RED)
    im.save(os.path.join(OUT, "05_stateflow.png"))

# ---------------------------------------------------------------- 06 解耦时序
def d06_decouple():
    im, d = canvas(960, 560)
    text(d, (30, 24), "两个时钟各自走: 渲染拉满, GIF 帧按素材节奏", 28, INK, True)
    # 旧方式
    text(d, (60, 84), "旧 (串行): 帧周期 = 翻页 + 画画 → 6.9fps, 猫像PPT", 19, RED, True)
    y1 = 120
    d.rectangle((60, y1, 200, y1+36), fill=RD_L, outline=RED); text(d, (130, y1+8), "翻页50ms", 15, INK, True, "ma")
    d.rectangle((204, y1, 380, y1+36), fill=RD_L, outline=RED); text(d, (292, y1+8), "画画37ms", 15, INK, True, "ma")
    text(d, (395, y1+8), "= 87ms/帧 → 11fps (再加tick误差 → 实测6.9)", 15, SUB)
    # 新方式
    text(d, (60, 200), "新 (解耦): 渲染循环拉满; fi 只在到期时前进", 19, GREEN, True)
    lane_y = 250
    text(d, (60, lane_y-6), "渲染时钟", 16, BLUE, True)
    for i in range(9):
        x = 60+i*95
        d.rectangle((x, lane_y+24, x+80, lane_y+62), fill=BLUE_L, outline=BLUE)
        text(d, (x+40, lane_y+34), "画", 17, BLUE, True, "ma")
    text(d, (60, lane_y+70), "每圈位置=sinf(绝对时间) → 圈圈不同 → 舞步丝滑 (~30fps)", 16, SUB)
    lane_y2 = lane_y+120
    text(d, (60, lane_y2-6), "GIF 帧时钟", 16, ORANGE, True)
    xs = [(60, "f3"), (255, "f4"), (540, "f5")]
    for x, s in xs:
        d.rectangle((x, lane_y2+24, x+180, lane_y2+62), fill=OR_L, outline=ORANGE)
        text(d, (x+90, lane_y2+34), f"显示 {s} (按delay_ms)", 15, INK, True, "ma")
    text(d, (60, lane_y2+70), "now ≥ fi_due 才翻页 —— 素材原速播放, 与渲染快慢无关", 16, SUB)
    text(d, (60, 510), "所以: fps 日志报的是渲染帧率(舞步的顺滑度), 翻页节奏由 GIF delay_ms 决定", 18, INK, True)
    im.save(os.path.join(OUT, "06_decouple.png"))

# ---------------------------------------------------------------- 07 延时提交
def d07_commit():
    im, d = canvas(960, 540)
    text(d, (30, 24), "交互: “等你说完再办事” —— 延时提交", 28, INK, True)
    # timeline
    ty = 150
    d.line((70, ty, 900, ty), fill=INK, width=3)
    taps = [(120, "tap1"), (230, "tap2"), (330, "tap3")]
    for x, s in taps:
        d.ellipse((x-10, ty-10, x+10, ty+10), fill=BLUE)
        text(d, (x, ty-42), s, 16, BLUE, True, "ma")
        text(d, (x, ty+16), "抬起沿计数", 13, SUB, anchor="ma")
    d.rectangle((360, ty-30, 640, ty+30), fill=YEL_L, outline=(225,200,90))
    text(d, (500, ty-14), "350ms 静默窗", 17, INK, True, "ma")
    text(d, (500, ty+8), "没再戳才算说完", 13, SUB, anchor="ma")
    d.ellipse((660-10, ty-10, 660+10, ty+10), fill=GREEN)
    text(d, (660, ty-42), "commit", 16, GREEN, True, "ma")
    text(d, (60, 230), "提交分流 (看攒了几个):", 19, INK, True)
    rows = [("1 次", "切舞步 still→sway→bounce→groove", BLUE_L),
            ("2 次", "切猫: 张望→碗装眼泪→双猫哭哭 (轮换表)", GR_L),
            ("≥3 次且在猫头", "彩蛋: 趴地哭+泪滴雨+灯快闪, 播完回现场", RD_L)]
    y = 270
    for k, v, c in rows:
        box(d, (70, y, 250, y+56), c); text(d, (160, y+16), k, 18, INK, True, "ma")
        text(d, (275, y+16), v, 17, SUB)
        y += 76
    text(d, (60, y+6), "代价: 单击延迟 350ms   收益: 三种手势零竞态, 永远不会判错", 18, INK, True)
    im.save(os.path.join(OUT, "07_commit.png"))

d01_layers(); d02_typewriter(); d03_band(); d04_pack(); d05_state(); d06_decouple(); d07_commit()
print("7 图已生成 →", OUT)
