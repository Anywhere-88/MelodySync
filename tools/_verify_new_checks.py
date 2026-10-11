# -*- coding: utf-8 -*-
"""
tools/_verify_new_checks.py —— 新增守门规则的「注入式反向验证」

为什么必须做：本工程已经被一条"永远通过的检查"坑过一次 —— 第一版
"早退发信号"检查在注入 bug 之后仍然报 0 处（判定窗口取宽了，把相邻的正确
代码算了进去）。一个永远通过的检查不只是没用，它比没有更糟：会给人虚假的
安全感。所以每条新规则都要**先造一个真 bug，确认它真的报错**，再还原。

本脚本只做临时注入并立即还原；不进版本控制也不影响构建。
用法：python tools/_verify_new_checks.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable

PV = os.path.join(ROOT, "src", "search", "sources", "PreviewSource.cpp")
DE = os.path.join(ROOT, "src", "download", "DownloadEngine.cpp")
FY = os.path.join(ROOT, "tools", "fetch_ytdlp.py")
DEP_YTDLP = os.path.join(ROOT, "build", "bin", "yt-dlp.exe")


def run_check():
    p = subprocess.run([PY, os.path.join(ROOT, "tools", "check_sources.py")],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    return p.stdout + p.stderr


def mutate(path, old, new, tmpdir):
    """就地改一处，返回还原用的回调。"""
    backup = os.path.join(tmpdir, os.path.basename(path) + ".bak")
    shutil.copy2(path, backup)
    with open(path, "r", encoding="utf-8") as fh:
        s = fh.read()
    assert s.count(old) >= 1, "注入点找不到: %r in %s" % (old, path)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(s.replace(old, new, 1))

    def restore():
        shutil.copy2(backup, path)
    return restore


CASES = []


def case(name, expect, path, old, new):
    CASES.append((name, expect, path, old, new))


# ---- yt-dlp：默认目标写回 build/bin（正是我踩过的那个坑） ----
case("fetch_ytdlp.py 默认写入 build/bin",
     "fetch_ytdlp.py 默认写入 build/bin",
     FY,
     'dest = args.dest or os.path.join(root, "bin", ASSET)',
     'dest = args.dest or os.path.join(root, "build", "bin", ASSET)')

# ---- 平台直链：不清空 downloadUrl ----
case("PreviewSource 又清空 downloadUrl",
     "downloadUrl.clear",
     PV,
     "            h.downloadUrl = a.url;",
     "            h.downloadUrl.clear();")

# ---- 平台直链：搜索阶段不再换直链 ----
case("搜索阶段不再换直链",
     "PlatformResolve::neteaseBatch",
     PV,
     "PlatformResolve::neteaseBatch(ids",
     "NeteaseBatchX(ids")

# ---- 平台直链：下载前不重解析 ----
case("下载前不重解析平台直链",
     "DownloadEngine 没有在下载前重新换直链",
     DE,
     "if (PlatformResolve::supports(task.sourceId) && !task.platformId.isEmpty())",
     "if (false && !task.platformId.isEmpty())")

# ---- 平台直链：不带 Referer ----
case("下载直链不带 Referer",
     "没带平台要求的 Referer",
     DE,
     "dlHeaders    = PlatformResolve::headersFor(task.sourceId);",
     "dlHeaders    = QStringList();")


def main():
    ok = True
    tmpdir = tempfile.mkdtemp(prefix="ms-inject-")
    print("=" * 72)
    print("注入式反向验证：每条新规则都必须能抓到对应的真 bug")
    print("=" * 72)

    base = run_check()
    if "ALL CHECKS PASSED" not in base:
        print("[前置失败] 未注入时检查就没通过，先修好再跑本脚本")
        return 2
    print("[前置] 未注入时全绿 ✅\n")

    # 1) 纯文件内容类注入
    for name, expect, path, old, new in CASES:
        restore = mutate(path, old, new, tmpdir)
        try:
            out = run_check()
        finally:
            restore()
        caught = expect in out
        print("  [%s] %s" % ("抓到" if caught else "漏掉", name))
        if not caught:
            ok = False
            print("         期望输出里出现：%s" % expect)
            print("         实际 RESULT 行：",
                  [l for l in out.splitlines() if l.startswith("RESULT")])
        # 还原后必须重新变绿，否则说明还原没做干净
        if "ALL CHECKS PASSED" not in run_check():
            ok = False
            print("         ⚠ 还原后没恢复全绿，请手工检查 %s" % path)

    # 2) 二进制副本不一致（单独处理：需要动 17MB 的文件）
    if os.path.isfile(DEP_YTDLP):
        with open(DEP_YTDLP, "ab") as fh:
            fh.write(b"\x00")           # 只动 build/bin 那份，bin/ 是干净的
        try:
            out = run_check()
        finally:
            shutil.copy2(os.path.join(ROOT, "bin", "yt-dlp.exe"), DEP_YTDLP)
        caught = "两份 yt-dlp.exe 不一致" in out
        print("  [%s] 两份 yt-dlp.exe 哈希不一致" % ("抓到" if caught else "漏掉"))
        if not caught:
            ok = False
        if "ALL CHECKS PASSED" not in run_check():
            ok = False
            print("         ⚠ 还原后没恢复全绿")
    else:
        print("  [跳过] build/bin/yt-dlp.exe 不存在")

    shutil.rmtree(tmpdir, ignore_errors=True)
    print("\n" + "=" * 72)
    print("RESULT: %s" % ("每条规则都真的能抓到 bug ✅" if ok else "有规则形同虚设 ❌"))
    print("=" * 72)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
