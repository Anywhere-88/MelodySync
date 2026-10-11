# -*- coding: utf-8 -*-
"""
tools/fetch_ytdlp.py —— 下载并校验 yt-dlp.exe

为什么需要这个脚本：
  之前用浏览器/单次 curl 拉的 yt-dlp.exe 是**截断**的（18,903,582 字节，
  比官方 17,840,399 还大，说明中间被塞了脏数据）。损坏的 PyInstaller 单文件
  包表现是启动即报：
      [PYI-xxx:ERROR] Failed to extract Cryptodome\\Cipher\\_ARC4.pyd:
                      decompression resulted in return code -3!
  而 DownloadEngine 会把它当成"下载失败"重试 3 次，用户只看到"下载失败"。

  所以这里做三件事：
    1) 用官方 SHA2-256SUMS 校验哈希 —— 只看文件大小是不够的；
    2) HTTP Range 断点续传 —— 本机网络/代理会把大文件截断在整数 MB 处；
    3) 落地后实际跑一次 `--version` —— 哈希对但跑不起来同样算失败。

用法：
    python tools/fetch_ytdlp.py                # 下载到 build/bin/
    python tools/fetch_ytdlp.py --dest X.exe   # 指定目标
    python tools/fetch_ytdlp.py --check        # 只体检现有文件
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

BASE = "https://github.com/yt-dlp/yt-dlp/releases/latest/download"
ASSET = "yt-dlp.exe"
SUMS = "SHA2-256SUMS"

# GitHub 的 release 资产走的是 release-assets.githubusercontent.com，
# 在国内（以及某些公司网络里）经常整段超时。这几个加速镜像只做"原样转发"，
# 下完照样用官方 SHA-256 校验，所以用镜像不会降低可信度。
# 顺序 = 优先级：先试直连，失败再轮镜像。
MIRRORS = [
    "https://gh-proxy.com/",
    "https://ghfast.top/",
    "https://ghproxy.net/",
]


def candidates(path_tail):
    """给出同一个资源的"直连 + 各镜像"URL 列表。"""
    url = f"{BASE}/{path_tail}"
    return [url] + [m + url for m in MIRRORS]

UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) MelodySync-fetch"}


def _open(url, extra=None):
    headers = dict(UA)
    if extra:
        headers.update(extra)
    return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60)


def _retry(fn, what, tries=8):
    """小请求（取哈希、探长度）也要重试。

    踩过的坑：这里原本是裸调用，结果代理偶发 RemoteDisconnected 直接抛异常，
    整个脚本以一个 Python traceback 收场 —— 用户看到的是调用栈而不是
    "网络不通，重跑一次"。凡是网络调用就都得有重试与兜底。
    """
    last = None
    for i in range(tries):
        try:
            return fn()
        except Exception as e:            # urllib 的异常种类多且不稳定，统一兜住
            last = e
            print(f"[fetch] {what} 第 {i + 1} 次失败：{e}")
            time.sleep(min(1.0 * (i + 1), 5))
    raise RuntimeError(f"{what} 连续 {tries} 次失败：{last}")


def official_sha256(asset=ASSET):
    """从官方 SHA2-256SUMS 里取出该文件的期望哈希。"""
    def _fetch_text():
        last = None
        for url in candidates(SUMS):
            try:
                with _open(url) as r:
                    text = r.read().decode("utf-8", "replace")
                if asset in text:
                    return text
                last = RuntimeError("内容里没有该文件的行")
            except Exception as e:
                last = e
                print(f"[fetch] SUMS 下载失败（{url.split('/')[2]}）：{e}")
        raise RuntimeError(f"所有线路都取不到 {SUMS}：{last}")
    text = _fetch_text()
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip("*") == asset:
            return parts[0].lower()
    raise RuntimeError(f"{SUMS} 里找不到 {asset} 的哈希")


def sha256_of(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def _remote_size(url):
    """拿远端总大小；服务器不支持 Range 时返回 0。"""
    try:
        with _open(url, {"Range": "bytes=0-0"}) as r:
            cr = r.headers.get("Content-Range")  # bytes 0-0/17840399
            if cr and "/" in cr:
                return int(cr.rsplit("/", 1)[1])
            return int(r.headers.get("Content-Length") or 0)
    except Exception:
        return 0


def download(url, dest, tries=40):
    """带断点续传的下载。

    关键点：单次请求被截断是**常态**（本机代理会做），所以不把它当错误，
    而是记下已下字节数、下一次带 Range 从断点继续。收到 416 说明
    我们已经比远端还长 —— 那是之前续传时多写的垃圾，直接把文件截到远端长度。
    """
    total = _remote_size(url)
    print(f"[fetch] 远端大小: {total or '未知'} 字节")

    for attempt in range(1, tries + 1):
        have = os.path.getsize(dest) if os.path.exists(dest) else 0
        if total and have == total:
            print(f"[fetch] 已完整（{have} 字节）")
            return True
        if total and have > total:
            print(f"[fetch] 本地 {have} > 远端 {total}，截断到远端长度")
            with open(dest, "r+b") as f:
                f.truncate(total)
            continue

        headers = {}
        mode = "wb"
        if have:
            headers["Range"] = f"bytes={have}-"
            mode = "ab"

        try:
            with _open(url, headers) as r:
                if have and r.status == 200:
                    # 服务器忽略了 Range：从头来，别把两段拼在一起
                    print("[fetch] 服务器不支持 Range，重新完整下载")
                    mode, have = "wb", 0
                elif have and r.status == 416:
                    print("[fetch] 416：已下完，转入校验")
                    return True
                with open(dest, mode) as f:
                    while True:
                        buf = r.read(256 << 10)
                        if not buf:
                            break
                        f.write(buf)
            got = os.path.getsize(dest)
            print(f"[fetch] 第 {attempt} 次收尾：{got}/{total or '?'} 字节")
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
            got = os.path.getsize(dest) if os.path.exists(dest) else 0
            print(f"[fetch] 第 {attempt} 次中断（已下 {got}）：{e}")
            time.sleep(min(1.5 * attempt, 4))
    return False


def probe_exe(path):
    """真跑一次 --version。哈希对但跑不起来 = 仍然不可用。

    超时给到 180 秒：yt-dlp 是 PyInstaller 单文件包，**首次**启动要把它自带的
    那一堆模块解压到 %TEMP%，在硬盘慢或杀软实时扫描的情况下能跑好几分钟。
    给 60 秒会把"第一次有点慢"误判成"这个文件坏了"，然后删掉刚下好的正确文件 ——
    比不检查还糟。
    """
    if not os.path.exists(path):
        return False, "文件不存在"
    try:
        p = subprocess.run([path, "--version"], capture_output=True, timeout=180)
    except subprocess.TimeoutExpired:
        # 超时不再当作损坏：文件留着，让用户/程序自己判断
        return False, "启动超过 180 秒仍未返回（可能是杀毒软件在扫描，可稍后重试）"
    except Exception as e:
        return False, f"启动异常：{e}"
    out = (p.stdout or b"").decode("utf-8", "replace").strip()
    err = (p.stderr or b"").decode("utf-8", "replace").strip()
    if re.match(r"^\d{4}\.\d{2}\.\d{2}", out):
        return True, out.splitlines()[0]
    return False, (err or out or f"退出码 {p.returncode}")[:400]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", default=None)
    ap.add_argument("--check", action="store_true", help="只体检，不下载")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # ⚠ 默认写到 **仓库的 bin/**，不是 build/bin/。
    # 踩过的坑：CMake 的 post-build 步骤会把 bin/yt-dlp.exe 复制到 build/bin/。
    # 如果只更新 build/bin/ 那一份，下次构建就会用 bin/ 里的旧文件把它盖回去 ——
    # 症状是"我明明修好了 yt-dlp，跑起来还是报已损坏"。
    # bin/ 才是唯一的源，build/bin/ 只是它的部署副本。
    dest = args.dest or os.path.join(root, "bin", ASSET)
    mirror = os.path.join(root, "build", "bin", ASSET)

    if args.check:
        for p in (dest, mirror):
            if os.path.exists(p):
                ok, info = probe_exe(p)
                print(("[OK]  " if ok else "[BAD] ") + p + " -> " + info)
        return 0

    want = official_sha256()
    print(f"[fetch] 目标: {dest}")
    print(f"[fetch] 期望 SHA-256: {want}")

    if os.path.exists(dest):
        got = sha256_of(dest)
        if got == want:
            ok, info = probe_exe(dest)
            if ok:
                print(f"[fetch] 已是正确版本 {info}，无需下载")
                _mirror(dest, mirror)
                return 0
            print(f"[fetch] 哈希正确但无法运行（{info}），重新下载")
        else:
            print(f"[fetch] 现有文件哈希不符（{got}），重新下载")
        os.remove(dest)

    # 逐条线路试：任一条能下全并哈希对上就算成功
    for i, url in enumerate(candidates(ASSET)):
        host = url.split("/")[2]
        print(f"[fetch] 线路 {i + 1}/{len(candidates(ASSET))}：{host}")
        if not download(url, dest):
            print(f"[fetch] {host} 未下完，换下一条线路")
            continue
        got = sha256_of(dest)
        if got == want:
            break
        print(f"[fetch] {host} 下到的内容哈希不对（{got}），换下一条线路")
        os.remove(dest)
    else:
        print("[fetch] 所有线路都不行。要么稍后重跑（支持续传），")
        print("        要么手工下载 yt-dlp.exe 放进 bin\\ 目录（见 bin\\放置说明.txt）")
        return 2

    ok, info = probe_exe(dest)
    if not ok:
        print(f"[fetch] 校验通过但无法运行：{info}")
        return 4

    print(f"[fetch] 完成：yt-dlp {info} @ {dest}（{os.path.getsize(dest)} 字节）")
    _mirror(dest, mirror)
    return 0


def _mirror(src, mirror):
    """顺手同步一份到 build/bin/，省得用户"改了却不生效"。"""
    if not os.path.isdir(os.path.dirname(mirror)):
        return
    try:
        with open(src, "rb") as a, open(mirror, "wb") as b:
            b.write(a.read())
        print(f"[fetch] 已同步到 {mirror}")
    except OSError as e:
        print(f"[fetch] 同步到 build/bin 失败（不影响，下次构建会自己复制）：{e}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[fetch] 已中断（文件保留，重跑会从断点续传）")
        sys.exit(130)
    except Exception as e:
        # 绝不把 traceback 扔给用户 —— 这里能出的错清一色是网络问题，
        # 而"重跑一次"就是正确的处理方式。
        print(f"\n[fetch] 失败：{e}")
        print("[fetch] 多为网络中断所致，直接重跑本脚本即可（已下部分会续传）")
        sys.exit(9)
