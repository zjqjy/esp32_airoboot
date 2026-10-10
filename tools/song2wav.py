#!/usr/bin/env python3
"""song2wav.py — 任意音频 → 板子可播的 16kHz/mono/16bit WAV (tools/assets_wav/song.wav)
6MB assets 分区预算: 16k*2B=32KB/s ≈ 上限 190s, 装不下整首时默认自动挑最响的
150s (副歌连段); 也可 --ss/--t 手动截。MP3/FLAC/M4A 解码: PATH 里的 ffmpeg,
或 Python imageio_ffmpeg 自带的 (无需安装)。

用法:
  python tools/song2wav.py 起风了.mp3            # 自动选副歌 150s
  python tools/song2wav.py 起风了.mp3 --ss 58 --t 150
之后打包烧录:
  python tools/gen_tts_wavs.py fat && tools\\flash_assets.bat
板上: 长按屏幕播放, 播放中再长按停止。
"""
import argparse
import array
import os
import shutil
import struct
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "assets_wav", "song.wav")
RATE = 16000
PEAK = 28000                 # 归一峰值 (留 15% 裕量, 喇叭端还有 GAIN 9dB)
BUDGET_S = 190               # 6MB 分区 - t1~t3 与 FAT 开销的保守秒数
AUTO_WIN_S = 150             # 自动副歌窗口


def find_ffmpeg():
    p = shutil.which("ffmpeg")
    if p:
        return p
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        return None


def read_input(path):
    """返回 (samples list[int], 采样率), 统一混成 mono 16bit."""
    if path.lower().endswith(".wav"):
        with wave.open(path, "rb") as w:
            rate, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
            raw = w.readframes(w.getnframes())
        if width != 2:
            sys.exit(f"输入 WAV 位深 {width*8}bit 不支持, 先转 16bit (或走 ffmpeg 路线)")
    else:
        ff = find_ffmpeg()
        if ff is None:
            sys.exit("没找到 ffmpeg (PATH 或 Python imageio_ffmpeg 都没有): "
                     "winget install ffmpeg 后重试")
        print(f"ffmpeg: {ff}")
        cmd = [ff, "-v", "error", "-i", path, "-f", "wav",
               "-acodec", "pcm_s16le", "-ac", "1", "-"]
        raw = subprocess.run(cmd, capture_output=True, check=True).stdout
        import io
        with wave.open(io.BytesIO(raw), "rb") as w:
            rate = w.getframerate()
            raw = w.readframes(w.getnframes())
        ch = 1
    n = len(raw) // 2 // ch
    samples = struct.unpack(f"<{n * ch}h", raw[: n * 2 * ch])
    if ch > 1:                          # ffmpeg 已 -ac 1, WAV 输入可能还带多声道
        samples = [sum(samples[i:i + ch]) // ch for i in range(0, len(samples), ch)]
    return samples, rate


def pick_loudest_window(samples, rate, win_s):
    """每秒算 RMS, 滑窗取均值最高的一段 (副歌/高潮判据)."""
    sec = len(samples) // rate
    if sec <= win_s:
        return 0, len(samples)
    rms = []
    for s in range(sec):
        seg = samples[s * rate:(s + 1) * rate]
        step = 4                        # 抽样 1/4 加速
        acc = sum(v * v for v in seg[::step])
        rms.append((acc / max(1, len(seg[::step]))) ** 0.5)
    win = win_s
    best, best_i = -1.0, 0
    cur = sum(rms[:win])
    for i in range(0, sec - win):
        if i > 0:
            cur += rms[i + win - 1] - rms[i - 1]
        if cur > best:
            best, best_i = cur, i
    print(f"自动选段: 第 {best_i}s ~ {best_i + win}s (窗口均值 RMS 最高)")
    return best_i * rate, (best_i + win) * rate


def resample(samples, rate):
    if rate == RATE:
        return samples
    n_out = len(samples) * RATE // rate
    out = array.array("h", [0]) * n_out
    step = (len(samples) - 1) / max(1, n_out - 1)
    for i in range(n_out):
        pos = i * step
        j = int(pos)
        frac = pos - j
        a, b = samples[j], samples[min(j + 1, len(samples) - 1)]
        out[i] = int(a + (b - a) * frac)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--ss", type=float, default=0, help="起始秒 (给 --ss/--t 则关闭自动选段)")
    ap.add_argument("--t", type=float, default=0, help="时长秒 (0=到结尾)")
    args = ap.parse_args()

    samples, rate = read_input(args.input)
    dur_all = len(samples) / rate
    print(f"输入: {dur_all:.1f}s @ {rate}Hz mono")

    if args.ss or args.t:
        a = int(args.ss * rate)
        b = a + int(args.t * rate) if args.t else len(samples)
        samples = samples[max(0, a):min(len(samples), b)]
    elif dur_all > BUDGET_S:
        a, b = pick_loudest_window(samples, rate, AUTO_WIN_S)
        samples = samples[a:b]

    samples = resample(samples, rate)

    peak = max((abs(s) for s in samples), default=0)
    if peak == 0:
        sys.exit("输入是静音?")
    g = PEAK / peak
    samples = [max(-32767, min(32767, int(s * g))) for s in samples]

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with wave.open(OUT, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(array.array("h", samples).tobytes())

    dur = len(samples) / RATE
    size = os.path.getsize(OUT)
    print(f"OK {OUT}")
    print(f"时长 {dur:.1f}s, 大小 {size/1024/1024:.2f}MB, 增益 x{g:.2f}")
    if dur > BUDGET_S:
        print(f"⚠️ 超出 6MB 分区预算 (≤{BUDGET_S}s), 烧录可能放不下 — 用 --ss/--t 截取一段")
    print("下一步: python tools/gen_tts_wavs.py fat && tools\\flash_assets.bat")


if __name__ == "__main__":
    main()
