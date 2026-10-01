#!/usr/bin/env python3
"""serial_recorder.py — 收 ESP32 录音机经串口导出的 PCM, 存 WAV, 可选本地语音识别 (faster-whisper).

用法 (在项目根目录, 用 ESP-IDF 自带 python 或装了 pyserial 的 python):
  python tools/serial_recorder.py --port COM3                # 只存 wav
  python tools/serial_recorder.py --port COM3 --asr          # 存 wav + 语音识别
  python tools/serial_recorder.py --port COM3 --asr --model tiny   # 更小的模型(更快, 略糊)

注意: 运行本脚本时关闭 idf.py monitor (串口独占)。每在屏幕录一段,
电脑自动存一个 rec_时间戳.wav; 加 --asr 则同时打印识别文字。

首次 --asr 会从 HuggingFace 下载模型: tiny≈75MB / base≈145MB / small≈500MB。
"""
import argparse
import base64
import os
import re
import wave
from datetime import datetime

try:
    import serial
except ImportError:
    raise SystemExit("缺 pyserial: pip install pyserial")

ap = argparse.ArgumentParser(description="ESP32 录音机串口收流 → WAV → 可选语音识别")
ap.add_argument("--port", default="COM3")
ap.add_argument("--baud", type=int, default=115200, help="USB-Serial/JTAG 下不影响实际速度")
ap.add_argument("--out", default=".", help="wav 保存目录")
ap.add_argument("--asr", action="store_true", help="识别语音 (需 pip install faster-whisper)")
ap.add_argument("--model", default="small", help="whisper 模型: tiny/base/small")
args = ap.parse_args()

_whisper = None

def transcribe(wav_path):
    global _whisper
    try:
        from faster_whisper import WhisperModel
    except ImportError:
        print("缺 faster-whisper: pip install faster-whisper (或去掉 --asr)")
        return
    if _whisper is None:
        print(f"加载 whisper 模型 {args.model} (首次会下载)...")
        _whisper = WhisperModel(args.model, device="cpu", compute_type="int8")
    print("识别中...")
    segments, info = _whisper.transcribe(wav_path, language="zh", vad_filter=True)
    text = "".join(s.text for s in segments).strip()
    print(f"识别结果: {text if text else '(未识别到语音)'}")
    with open(os.path.splitext(wav_path)[0] + ".txt", "w", encoding="utf-8") as f:
        f.write(text)

B64_RE = re.compile(rb"^[A-Za-z0-9+/=]+$")

ser = serial.Serial(args.port, args.baud, timeout=1)
print(f"已打开 {args.port} — 等待录音机导出 (屏幕单击录音 → 再击停止)...")
collecting = False
payload = bytearray()
frames = 0

while True:
    line = ser.readline()
    if not line:
        continue
    line = line.strip()
    if line.startswith(b"===XZ_PCM "):
        m = re.search(rb"n=(\d+)", line)
        frames = int(m.group(1)) if m else 0
        collecting = True
        payload = bytearray()
        print(f"收到导出头 ({frames} 帧), 接收中...")
    elif collecting and line.startswith(b"===XZ_PCM_END==="):
        pcm = base64.b64decode(bytes(payload))
        fn = os.path.join(args.out, datetime.now().strftime("rec_%Y%m%d_%H%M%S.wav"))
        with wave.open(fn, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(16000)
            w.writeframes(pcm)
        print(f"已保存 {fn} ({len(pcm)} 字节, {frames} 帧)")
        collecting = False
        if args.asr and frames > 3200:      # >0.2s 才值得识别
            transcribe(fn)
        print("继续等待下一段...")
    elif collecting and line and B64_RE.match(line):
        payload += line
