#!/usr/bin/env python3
"""asr_server.py — 接收板子 POST 的 PCM, 存 WAV, faster-whisper 识别.
自带网页界面: 浏览器打开 http://电脑IP:8765/ 即可看到识别记录卡片
(时间/时长/识别文字/在线播放音频), 自动刷新, 无需任何前端依赖。

用法:
  python tools/asr_server.py                 # small 模型
  python tools/asr_server.py --model tiny
板子侧 CONFIG_S6_ASR_URL 指向启动时打印的地址。
"""
import argparse
import io
import json
import os
import socket
import threading
import time
import wave
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=8765)
ap.add_argument("--model", default="small", help="tiny/base/small")
args = ap.parse_args()

RECORDS = []          # [{ts, dur, text, wav_id}]
LOCK = threading.Lock()
WAV_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "asr_wavs")
os.makedirs(WAV_DIR, exist_ok=True)

_whisper = None

def get_model():
    global _whisper
    if _whisper is None:
        from faster_whisper import WhisperModel
        print(f"加载 whisper 模型 {args.model} (首次运行会下载)...", flush=True)
        _whisper = WhisperModel(args.model, device="cpu", compute_type="int8")
    return _whisper

def pcm_to_wav_bytes(pcm):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm)
    return buf.getvalue()

def process(pcm, wav_id):
    """识别 + 登记 (后台线程跑, 不阻塞 HTTP 响应之外的请求)"""
    wav_bytes = pcm_to_wav_bytes(pcm)
    fn = os.path.join(WAV_DIR, f"{wav_id}.wav")
    with open(fn, "wb") as f:
        f.write(wav_bytes)
    text = ""
    try:
        m = get_model()
        tmp = io.BytesIO(wav_bytes)
        tmp.name = "r.wav"
        segments, info = m.transcribe(tmp, language="zh", vad_filter=True)
        text = "".join(s.text for s in segments).strip()
    except Exception as e:
        print("识别出错:", e, flush=True)
    rec = {
        "ts": datetime.now().strftime("%H:%M:%S"),
        "dur": round(len(pcm) / 2 / 16000, 1),
        "text": text or "(未识别到语音)",
        "wav_id": wav_id,
    }
    with LOCK:
        RECORDS.insert(0, rec)
        del RECORDS[50:]
    print(f"[ASR] {wav_id} {rec['dur']}s: {rec['text']}", flush=True)

PAGE = """<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>小猫语音识别</title>
<style>
  body { background:#0e1116; color:#e8eaed; font-family:"Microsoft YaHei",sans-serif;
         max-width:720px; margin:0 auto; padding:20px; }
  h1 { font-size:20px; color:#9aa0a6; }
  .card { background:#1f1f1f; border-radius:14px; padding:14px 18px; margin:12px 0; }
  .ts { color:#6f8af6; font-size:13px; }
  .dur { color:#9aa0a6; font-size:13px; margin-left:10px; }
  .text { font-size:22px; margin:8px 0; color:#ffd54d; }
  audio { width:100%; margin-top:6px; }
  .empty { color:#5f6368; text-align:center; padding:60px 0; }
  #st { color:#3ddc84; font-size:13px; }
</style></head><body>
<h1>🐱 小猫语音识别 <span id="st">● 实时</span></h1>
<div id="list"><div class="empty">等一段录音…（对板子单击录音 → 说话 → 再单击）</div></div>
<script>
let last = "";
async function refresh() {
  try {
    const r = await fetch("/records");
    const j = await r.json();
    const sig = JSON.stringify(j.map(x => x.wav_id + x.text));
    if (sig === last) return;
    last = sig;
    const el = document.getElementById("list");
    el.innerHTML = j.length ? "" : '<div class="empty">等一段录音…</div>';
    for (const rec of j) {
      const div = document.createElement("div");
      div.className = "card";
      div.innerHTML = `<span class="ts">${rec.ts}</span><span class="dur">${rec.dur}s</span>
        <div class="text">${rec.text}</div>
        <audio controls preload="none" src="/wav/${rec.wav_id}.wav"></audio>`;
      el.appendChild(div);
    }
  } catch (e) { document.getElementById("st").textContent = "● 服务离线"; }
}
setInterval(refresh, 1500);
refresh();
</script></body></html>"""

class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        ln = int(self.headers.get("Content-Length", 0))
        pcm = self.rfile.read(ln)
        print(f"收到 {ln} 字节 PCM", flush=True)
        wav_id = datetime.now().strftime("%Y%m%d_%H%M%S")
        resp = '{"text":"识别中"}'.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)
        threading.Thread(target=process, args=(pcm, wav_id), daemon=True).start()

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            body = PAGE.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/records":
            with LOCK:
                body = json.dumps(RECORDS, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path.startswith("/wav/"):
            wav_id = os.path.basename(self.path)
            fn = os.path.join(WAV_DIR, wav_id)
            if os.path.isfile(fn):
                with open(fn, "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "audio/wav")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_response(404)
                self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()

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
    print(f"ASR 服务已启动", flush=True)
    print(f"  网页界面:  http://{ip}:{args.port}/   ← 浏览器打开这个看结果", flush=True)
    print(f"  板子地址:  http://{ip}:{args.port}/asr", flush=True)
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()
