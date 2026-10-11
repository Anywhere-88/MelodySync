#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""跑一遍程序内置的桥接自检（--selftest），并把结果整理出来。

它做三件事：
  1. 清空旧日志，用 subprocess 启动 MelodySync.exe --selftest；
  2. 轮询日志，直到出现自检结束标记（或超时）；
  3. 打印桥接实测结果。

为什么用脚本托管进程，而不是在 shell 里 `exe ... &`
==================================================
实测在 bash 里用 `&` 启动这个 GUI 程序，它会在两三秒后"正常退出"
（app.exec() 自己返回）。同一个 exe 由 Python 的 subprocess 启动则稳定常驻。
所以自动化必须由脚本托管，别用 shell 的 `&`。

用法
====
    python tools/selftest.py
    python tools/selftest.py --query "周杰伦 晴天"
    python tools/selftest.py --playlist "https://music.163.com/#/playlist?id=2805215308"
    python tools/selftest.py --no-search --no-playlist     # 只测基础桥接
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "build", "bin", "MelodySync.exe")
LOG = os.path.join(os.environ.get("APPDATA", ""), "MelodySync", "MelodySync",
                   "logs", "melodysync.log")

DONE_MARK = "========== 结束 =========="


def read_log():
    if not os.path.isfile(LOG):
        return ""
    for enc in ("utf-8", "gbk", "latin-1"):
        try:
            with open(LOG, "r", encoding=enc, errors="replace") as fh:
                return fh.read()
        except Exception:
            continue
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", default="周杰伦 晴天")
    ap.add_argument("--playlist", default="https://music.163.com/#/playlist?id=2805215308")
    ap.add_argument("--no-search", action="store_true")
    ap.add_argument("--no-playlist", action="store_true")
    ap.add_argument("--timeout", type=float, default=200.0)
    args = ap.parse_args()

    if not os.path.isfile(EXE):
        sys.exit("找不到 %s，先跑 python tools/build.py" % EXE)

    try:
        os.remove(LOG)
    except OSError:
        pass

    cmd = [EXE, "--selftest"]
    if not args.no_search:
        cmd += ["--selftest-query", args.query]
    if not args.no_playlist:
        cmd += ["--selftest-playlist", args.playlist]

    print("启动:", " ".join('"%s"' % c if " " in c else c for c in cmd))
    # stdin 一并接管：让程序不依赖父 shell 的任何终端状态
    proc = subprocess.Popen(cmd, cwd=os.path.dirname(EXE),
                            stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    deadline = time.time() + args.timeout
    text = ""
    done = False
    try:
        while time.time() < deadline:
            time.sleep(1.0)
            text = read_log()
            if DONE_MARK in text:
                done = True
                break
            if proc.poll() is not None:
                time.sleep(1.0)             # 进程结束，再读一次日志
                text = read_log()
                done = DONE_MARK in text
                break
    finally:
        if proc.poll() is None:
            proc.terminate()
            time.sleep(1.5)
            if proc.poll() is None:
                # 自检模式下窗口会拒绝 WM_CLOSE（"自检进行中"），
                # terminate() 都无效时只能强杀，别把 exe 占着导致下次 LNK1104。
                try:
                    subprocess.run(["taskkill", "/F", "/PID", str(proc.pid)],
                                   capture_output=True, timeout=10)
                except Exception:
                    pass

    # ⚠ 假绿守门：没等到"结束"标记 = 自检脚本根本没跑完（页面 JS 卡死 /
    # 渲染进程挂起等）。这时"没有发现可疑项"毫无意义，必须按失败处理。
    # 曾经就在这种状态下报"0 可疑"，实际连一条结果都没产生过。
    if not done:
        print("\n自检未完成（没等到结束标记）——按失败处理，不当作通过。日志最后 20 行：")
        for l in text.splitlines()[-20:]:
            print("  " + l)
        return 1

    lines = [l for l in text.splitlines() if "Selftest" in l or "Playlist]" in l]
    if not lines:
        print("\n没有自检输出。日志最后 20 行：")
        for l in text.splitlines()[-20:]:
            print("  " + l)
        return 1

    print()
    bad = 0
    for l in lines:
        if "Selftest" in l:
            body = l.split("Selftest]", 1)[1].strip()
        else:
            body = l.strip()
        if "JS-ERR" in body or "FAIL" in body or "null" == body.split("=")[-1].strip():
            bad += 1
        print("  " + body)

    print("\n可疑项（JS-ERR / FAIL / null）: %d" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
