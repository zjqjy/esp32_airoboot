#!/usr/bin/env python3
"""asr_server.py — 接收板子 POST 的 PCM (16kHz/mono/s16le 裸数据), 存 WAV,
faster-whisper 识别, 返回 JSON {"text": "..."} 给板子显示.

用法:
  python tools/asr_server.py                 # 8765 端口, small 模型
  python tools/asr_server.py --model tiny    # 更小更快
首次识别会下载 whisper 模型; Windows 首次运行需在防火墙弹窗里允许访问。
板子侧 CONFIG_S6_ASR_URL 指向本机局域网 IP (启动时会打印)。
"""
import argparse
import io
import json
import socket
import wave
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=8765)
ap.add_argument("--model", default="small", help="tiny/base/small")
args = ap.parse_args()

_whisper = None

def get_model():
    global _whisper
    if _whisper is None:
        from faster_whisper import WhisperModel
        print(f"加载 whisper 模型 {args.model} (首次运行会下载)...")
        _whisper = WhisperModel(args.model, device="cpu", compute_type="int8")
    return _whisper

def pcm_to_wav(pcm):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm)
    return buf.getvalue()

def transcribe(pcm):
    m = get_model()
    wav = pcm_to_wav(pcm)
    fn = datetime.now().strftime("rec_%Y%m%d_%H%M%S.wav")
    with open(fn, "wb") as f:
        f.write(wav)
    segments, info = m.transcribe(fn, language="zh", vad_filter=True)
    text = "".join(s.text for s in segments).strip()
    print(f"[ASR] {fn}: {text if text else '(未识别到语音)'}")
    return text

class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        ln = int(self.headers.get("Content-Length", 0))
        pcm = self.rfile.read(ln)
        print(f"收到 {ln} 字节 PCM, 识别中...")
        try:
            text = transcribe(pcm)
        except Exception as e:
            text = ""
            print("识别出错:", e)
        resp = json.dumps({"text": text}, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def do_GET(self):
        resp = b'{"ok":true}'
        self.send_response(200)
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, *a):
        pass

def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
    finally:
        s.close()
    return ip

if __name__ == "__main__":
    ip = lan_ip()
    print(f"ASR 服务已启动: http://{ip}:{args.port}/asr")
    print(f"板子 CONFIG_S6_ASR_URL = http://{ip}:{args.port}/asr")
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()
