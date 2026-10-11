#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SSH 反向隧道守护：自动重连 + 域名变化时刷新二维码。

解决什么问题
  localhost.run 的**匿名**隧道在重连后可能分配到新域名，旧链接会返回
  `503 <h1>no tunnel here :(</h1>`。如果你已经把链接发出去了，对方就会看到 503。
  本脚本长驻：捕获当前域名 → 写进 url 文件 → 自动重绘二维码 PNG。
  于是**桌面上那张二维码永远指向有效链接**，你只需要在域名变化时把新链接补发给对方。

为什么不用 `nohup ssh ... &`
  在这种工具化的 shell 里，后台进程会随命令返回被带走；必须由调用方
  （Bash 工具的 run_in_background）托管本脚本的生命周期。

用法（用隔离环境里的 python 跑，因为要 qrcode + Pillow）
  <venv>\\Scripts\\python.exe tools\\tunnel_guard.py --port 18800 \\
      --qr-out "%USERPROFILE%\\Desktop\\MelodySync-share-qr-public.png"
按 Ctrl+C 或结束该后台任务即断开隧道。
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

DOMAIN_RE = re.compile(r"https://([a-z0-9][a-z0-9-]*)\.lhr\.life")

# ssh 的常见增强选项：不交互、不问 host key、保活
SSH_OPTS = [
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=NUL",     # Windows 空设备；Linux/macOS 改成 /dev/null
    "-o", "ServerAliveInterval=30",
    "-o", "ServerAliveCountMax=3",
    "-o", "ExitOnForwardFailure=yes",
    "-o", "LogLevel=ERROR",
]


def log(msg: str) -> None:
    print(f"[guard] {msg}", flush=True)


def regenerate_qr(url: str, qr_out: Path, preset: str) -> None:
    """复用同目录下的 make_share_qr.py 重绘二维码（保持单一实现）。"""
    script = Path(__file__).with_name("make_share_qr.py")
    if not script.is_file():
        log(f"找不到 {script}，跳过二维码刷新")
        return
    try:
        r = subprocess.run(
            [sys.executable, str(script), "--url", url, "--preset", preset, "--out", str(qr_out)],
            capture_output=True, text=True, timeout=60,
        )
        if r.returncode == 0:
            log(f"二维码已刷新 -> {qr_out}")
        else:
            log(f"二维码刷新失败: {(r.stderr or r.stdout).strip()[:200]}")
    except Exception as e:  # noqa: BLE001
        log(f"二维码刷新异常: {e}")


def main() -> int:
    ap = argparse.ArgumentParser(description="SSH 反向隧道守护（自动重连 + 刷新二维码）")
    ap.add_argument("--port", type=int, default=18800, help="本地被转发的端口")
    ap.add_argument("--host", default="nokey@localhost.run", help="隧道服务端")
    ap.add_argument("--qr-out", default=None, help="二维码 PNG 输出路径")
    ap.add_argument("--url-file", default=None, help="把当前链接写到这个文本文件")
    ap.add_argument("--preset", default="public", choices=["lan", "public"])
    args = ap.parse_args()

    qr_out = Path(args.qr_out) if args.qr_out else None
    url_file = Path(args.url_file) if args.url_file else None

    current = None
    attempt = 0
    while True:
        attempt += 1
        log(f"第 {attempt} 次建立隧道：-R 80:localhost:{args.port} {args.host}")
        cmd = ["ssh"] + SSH_OPTS + ["-R", f"80:localhost:{args.port}", args.host]
        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding="utf-8", errors="replace", bufsize=1,
            )
        except FileNotFoundError:
            log("找不到 ssh 可执行文件（需要 OpenSSH 客户端）")
            return 1

        try:
            assert proc.stdout is not None
            for line in proc.stdout:
                # 忽略隧道服务端画的那张 ANSI 二维码，只认 URL
                m = DOMAIN_RE.search(line)
                if not m:
                    continue
                url = m.group(0)
                if url == current:
                    continue
                current = url
                log("=" * 62)
                log(f"当前链接：{url}")
                log("=" * 62)
                if url_file:
                    url_file.write_text(url + "\n", encoding="utf-8")
                if qr_out:
                    regenerate_qr(url, qr_out, args.preset)
        except Exception as e:  # noqa: BLE001
            log(f"读取隧道输出异常: {e}")
        finally:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        log("隧道已断开，3 秒后重连…")
        time.sleep(3)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[guard] 已停止")
