# -*- coding: utf-8 -*-
"""vcpkg 断网救急：github.com 不通时，经 gh-proxy 镜像补齐 downloads/ 缺失源码包。

用法: python tools/fetch_vcpkg_sources.py [--triplet x64-windows]
循环: 跑 vcpkg install --debug → 解析 "Downloading <url> -> <name>" →
      对 github.com 域名的走 https://gh-proxy.com/ 镜像下载到 downloads/<name>
      → 重跑，直到装完或没有可补的下载。
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# vcpkg 本体在工作空间层（run_vcpkg._VCPKG_DEFAULT = 上上级/vcpkg），别把缓存塞进仓库
DL = os.path.join(os.path.dirname(ROOT), "vcpkg", "downloads")
LOG = os.path.join(ROOT, "build", "_vcpkg_fetch.log")
PROXY = "https://gh-proxy.com/"
MIN_SIZE = 10 * 1024  # 小于 10KB 基本是错误页（utfcpp 这类小包约 39KB）
DL_RE = re.compile(r"Downloading (https://\S+) -> (\S+)")


def looks_real(path):
    """校验是真 gzip/zip 而不是 200 状态码包着的 HTML 错误页。"""
    try:
        with open(path, "rb") as f:
            head = f.read(2)
        return head in (b"\x1f\x8b", b"PK")  # gzip / zip 魔数
    except OSError:
        return False

PY = sys.executable
RUN = os.path.join(ROOT, "tools", "run_vcpkg.py")


def curl(url, dst):
    # -L 跟随跳转；--fail 让 HTTP 错误码直接失败而不是存错误页
    r = subprocess.run(
        ["curl", "-sL", "--fail", "--connect-timeout", "20", "--retry", "3",
         "--retry-delay", "2", "--max-time", "1800", "-o", dst, PROXY + url],
        capture_output=True, timeout=1900)
    return r.returncode == 0 and os.path.exists(dst) and os.path.getsize(dst) >= MIN_SIZE


def main():
    os.makedirs(DL, exist_ok=True)
    triplet = "x64-windows"
    if "--triplet" in sys.argv:
        triplet = sys.argv[sys.argv.index("--triplet") + 1]

    for round_no in range(1, 30):
        with open(LOG, "w", encoding="utf-8") as f:
            p = subprocess.run([PY, RUN, "install", "--triplet", triplet, "--debug"],
                               stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)
        log = open(LOG, encoding="utf-8", errors="replace").read()
        if p.returncode == 0:
            print("ROUND %d: vcpkg install 成功" % round_no)
            return 0
        # 收集本轮所有 github 下载请求
        wanted = DL_RE.findall(log)
        todo = []
        for url, name in wanted:
            if "github.com" not in url:
                continue
            dst = os.path.join(DL, name)
            if os.path.exists(dst) and os.path.getsize(dst) >= MIN_SIZE and looks_real(dst):
                continue  # 已有且是真包
            todo.append((url, name, dst))
        if not todo:
            print("ROUND %d: 无 github 下载可补，但 install 仍失败 exit=%d" % (round_no, p.returncode))
            tail = [l for l in log.splitlines() if l.strip()][-25:]
            print("\n".join(tail))
            return 1
        print("ROUND %d: 需补 %d 个源码包" % (round_no, len(todo)))
        for url, name, dst in todo:
            print("  ↓ %s (%.1f MB)" % (name, 0))
            ok = curl(url, dst)
            if ok:
                ok = looks_real(dst)  # 200 的 HTML 错误页在这里拦下
            size = os.path.getsize(dst) if os.path.exists(dst) else 0
            print("    %s %d 字节" % ("OK" if ok else "FAIL", size))
            if not ok and os.path.exists(dst):
                os.remove(dst)  # 清掉错误页
    print("30 轮仍未装完，放弃")
    return 1


if __name__ == "__main__":
    sys.exit(main())
