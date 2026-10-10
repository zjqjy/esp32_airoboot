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
SESSION = bytearray()  # 流式会话: /chunk 累积, X-Final 识别后清空
LOCK = threading.Lock()
WAV_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "asr_wavs")
os.makedirs(WAV_DIR, exist_ok=True)

_whisper = None

def get_model():
    """FunASR Paraformer (中文优, CPU 亲和) — 复用 media/cut 技能同款模型配置.
    模型走 ModelScope 国内 CDN, 首跑共下载 ~1.2G 后走本地缓存 (下载时关 VPN)."""
    global _whisper
    if _whisper is None:
        from funasr import AutoModel
        print("加载 FunASR paraformer-zh (首次运行下载 ~1.2G, 之后走缓存)...", flush=True)
        _whisper = AutoModel(model="paraformer-zh", vad_model="fsmn-vad",
                             punc_model="ct-punc-c", disable_update=True)
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
    """识别 + 登记 + 返回文字. X-Final 同步调用: 响应直出真文字给板子
    (S7-C; ThreadingHTTPServer 多线程, 识别期间网页轮询/分块上传不受阻)"""
    wav_bytes = pcm_to_wav_bytes(pcm)
    fn = os.path.join(WAV_DIR, f"{wav_id}.wav")
    with open(fn, "wb") as f:
        f.write(wav_bytes)
    text = ""
    try:
        m = get_model()
        res = m.generate(input=fn, batch_size_s=30)
        text = "".join(r.get("text", "") for r in res).strip()
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
    return rec["text"]

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
    const st = document.getElementById("st");
    st.textContent = j.live > 0 ? `● 正在传输 ${(j.live / 1024).toFixed(0)} KB` : "● 实时";
    st.style.color = j.live > 0 ? "#ffd54d" : "#3ddc84";
    const sig = JSON.stringify(j.records.map(x => x.wav_id + x.text)) + j.live;
    if (sig === last) return;
    last = sig;
    const el = document.getElementById("list");
    el.innerHTML = j.records.length ? "" : '<div class="empty">等一段录音…（对板子单击录音 → 说话 → 再单击）</div>';
    for (const rec of j.records) {
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
        body = self.rfile.read(ln)
        is_chunk = self.headers.get("X-Chunk") == "1"
        is_final = self.headers.get("X-Final") == "1"
        if is_chunk:
            with LOCK:
                SESSION.extend(body)
            print(f"[流式] +{ln}B, 会话累计 {len(SESSION)}B", flush=True)
            resp = b'{"ok":true}'
            self.send_response(200)
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)
            return
        # /asr: X-Final = 识别流式会话; 无头 = 整段模式 (旧固件兼容)
        pcm = bytes(SESSION) if (is_final and SESSION) else body
        with LOCK:
            SESSION.clear()
        print(f"收到 {ln}B, 识别 {len(pcm)}B PCM...", flush=True)
        wav_id = datetime.now().strftime("%Y%m%d_%H%M%S")
        text = process(pcm, wav_id)     # 同步识别 (S7-C): 板子气泡直接拿真文字, 不再停"识别中"
        resp = json.dumps({"text": text}, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

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
                body = json.dumps({"records": RECORDS, "live": len(SESSION)},
                                  ensure_ascii=False).encode("utf-8")
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
