#!/usr/bin/env python3
"""gen_tts_wavs.py — 生成 S5 台词占位 WAV (16kHz/16bit/mono PCM)
真正的台词用在线 TTS 合成后同名替换 t1/t2/t3.wav, 再跑一次本脚本第二步(fatfsgen)即可.
占位音效: t1=上升双音(开工)  t2=三连短哔(进行中)  t3=下行三音(完成)
用法:
  python gen_tts_wavs.py wav       # 生成 tools/assets_wav/t1~t3.wav
  python gen_tts_wavs.py fat       # 打包 FAT 镜像 tools/assets_fat.img (供 parttool 烧录)
"""
import math
import os
import struct
import subprocess
import sys
import wave

HERE = os.path.dirname(__file__)
WAV_DIR = os.path.join(HERE, "assets_wav")
IMG = os.path.join(HERE, "assets_fat.img")
FATFSGEN = r"D:\.espressif\v6.1\esp-idf\components\fatfs\fatfsgen.py"
RATE = 16000


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767))))
                          for s in samples)
        w.writeframes(frames)


def tone(freqs, ms, amp=0.6):
    """同时叠加几个频率, 包 10ms 淡入淡出防咔嗒."""
    n = RATE * ms // 1000
    out = []
    for i in range(n):
        t = i / RATE
        v = sum(math.sin(2 * math.pi * f * t) for f in freqs) / len(freqs)
        fade = min(1.0, i / (RATE * 0.01), (n - i) / (RATE * 0.01))
        out.append(v * amp * fade)
    return out


def gen_wavs():
    os.makedirs(WAV_DIR, exist_ok=True)
    t1, t2, t3 = [], [], []
    for f in (523.25, 783.99):                       # C5→G5 上行双音, 各 180ms
        t1 += tone([f], 180)
    t2 = (tone([880], 90) + tone([0], 60) +
          tone([880], 90) + tone([0], 60) + tone([880], 90))
    for f in (783.99, 659.25, 523.25):               # G5→E5→C5 下行, 各 160ms
        t3 += tone([f], 160)
    for name, seg in (("t1.wav", t1), ("t2.wav", t2), ("t3.wav", t3)):
        write_wav(os.path.join(WAV_DIR, name), seg)
        print("wav:", os.path.join(WAV_DIR, name), f"{len(seg)*1000//RATE}ms")


def gen_fat():
    cmd = [sys.executable, FATFSGEN,
           "--partition_size", str(6 * 1024 * 1024),   # S7: 扩到 assets 分区全量 6M (放歌曲, ~190s)
           "--output_file", IMG,
           WAV_DIR]
    subprocess.run(cmd, check=True)
    print("fat image:", IMG, os.path.getsize(IMG), "bytes")


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    if mode in ("wav", "all"):
        gen_wavs()
    if mode in ("fat", "all"):
        gen_fat()
