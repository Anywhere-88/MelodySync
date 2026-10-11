#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""单文件 HTTP 分发服务：浏览器里点开链接即可下载指定文件。

为什么不直接用 `python -m http.server`：
  它会把整个目录（比如桌面）暴露出去。这里只暴露你指定的那一个文件，
  其余路径一律 404。

特性
  - 只服务一个文件，支持中文/空格文件名
  - 支持 HTTP Range 断点续传与多线程下载器
  - HEAD 请求、下载速率统计（仅日志，不影响传输）

用法
  python tools/serve_zip.py --file C:\\Users\\me\\Desktop\\a.zip
  python tools/serve_zip.py --file a.zip --port 18800 --bind 0.0.0.0
  python tools/serve_zip.py --file a.zip --open   # 启动后自动打开浏览器
"""

import argparse
import os
import re
import socket
import sys
import threading
import time
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

CHUNK = 1 << 20  # 1MB


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}GB"


def lan_ip() -> str:
    """取本机对外可达的 IPv4（用 UDP 试探路由，不实际发包）。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.1.1", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def build_handler(target: Path, download_name: str, stats: dict):
    size = target.stat().st_size
    quoted = urllib.parse.quote(download_name)
    # 兼容老浏览器：ASCII 回退名 + UTF-8 名双写
    disposition = f"attachment; filename=\"{download_name.encode('ascii', 'ignore').decode() or 'download.bin'}\"; filename*=UTF-8''{quoted}"

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "MelodySyncShare/1.0"

        def log_message(self, fmt, *args):
            sys.stderr.write("[share] %s %s\n" % (self.address_string(), fmt % args))
            sys.stderr.flush()

        # ---- 路径校验：只放行根路径和文件名本身 ----
        def _resolve(self) -> bool:
            raw = self.path.split("?", 1)[0]
            if raw in ("/", "/" + download_name, "/" + quoted):
                return True
            try:
                if urllib.parse.unquote(raw) == "/" + download_name:
                    return True
            except Exception:
                pass
            return False

        def _send_404(self):
            body = "404 Not Found — 本服务只提供 1 个文件：/" + quoted + "\n"
            b = body.encode("utf-8")
            self.send_response(404)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(b)

        def _parse_range(self):
            """返回 (start, end, is_partial)；非法返回 None。"""
            hdr = self.headers.get("Range")
            if not hdr:
                return 0, size - 1, False
            m = re.match(r"bytes=(\d*)-(\d*)$", hdr.strip())
            if not m:
                return None
            a, b = m.group(1), m.group(2)
            if a == "" and b == "":
                return None
            if a == "":  # bytes=-N 尾部 N 字节
                n = int(b)
                if n <= 0:
                    return None
                return max(0, size - n), size - 1, True
            start = int(a)
            end = int(b) if b else size - 1
            if start >= size or end < start:
                return None
            return start, min(end, size - 1), True

        def _respond(self, with_body: bool):
            if not self._resolve():
                self._send_404()
                return
            rng = self._parse_range()
            if rng is None:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            start, end, partial = rng
            length = end - start + 1

            self.send_response(206 if partial else 200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Disposition", disposition)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length))
            if partial:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            if not with_body:
                return

            stats["hits"] += 1
            sent = 0
            t0 = time.time()
            try:
                with open(target, "rb") as f:
                    f.seek(start)
                    remain = length
                    while remain > 0:
                        buf = f.read(min(CHUNK, remain))
                        if not buf:
                            break
                        self.wfile.write(buf)
                        sent += len(buf)
                        remain -= len(buf)
            except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
                sys.stderr.write(f"[share] 客户端中断（已传 {human(sent)}/{human(length)}）\n")
            finally:
                dt = max(time.time() - t0, 1e-6)
                stats["bytes"] += sent
                sys.stderr.write(
                    f"[share] 完成 {human(sent)} / {human(length)} 用时 {dt:.1f}s ({human(sent / dt)}/s)\n"
                )
                sys.stderr.flush()

        def do_GET(self):
            self._respond(True)

        def do_HEAD(self):
            self._respond(False)

    return Handler


def main() -> int:
    ap = argparse.ArgumentParser(description="单文件 HTTP 分发服务")
    ap.add_argument("--file", required=True, help="要分发的文件路径")
    ap.add_argument("--port", type=int, default=18800)
    ap.add_argument("--bind", default="0.0.0.0", help="绑定地址，默认全部网卡")
    ap.add_argument("--open", action="store_true", help="启动后自动打开浏览器")
    args = ap.parse_args()

    target = Path(args.file)
    if not target.is_absolute():
        target = (Path.cwd() / target).resolve()
    if not target.is_file():
        print(f"[X] 文件不存在: {target}")
        return 1

    name = target.name
    stats = {"hits": 0, "bytes": 0}
    handler = build_handler(target, name, stats)
    httpd = ThreadingHTTPServer((args.bind, args.port), handler)
    httpd.daemon_threads = True

    q = urllib.parse.quote(name)
    print(f"[OK] 分发中: {target}")
    print(f"     大小  : {human(target.stat().st_size)}")
    print()
    print(f"     本机   http://127.0.0.1:{args.port}/{q}")
    print(f"     局域网 http://{lan_ip()}:{args.port}/{q}")
    print()
    print("     按 Ctrl+C 停止服务")
    sys.stdout.flush()

    if args.open:
        threading.Thread(
            target=lambda: (time.sleep(0.6), webbrowser.open(f"http://127.0.0.1:{args.port}/{q}")),
            daemon=True,
        ).start()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[!] 服务已停止")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
