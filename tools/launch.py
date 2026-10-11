# -*- coding: utf-8 -*-
"""
MelodySync 智能启动器
=====================

为什么需要它：
    工程编译产物是 build/bin/MelodySync.exe。如果直接用快捷方式指向那个 exe，
    在「还没编译」的时候双击只会弹一个 Windows 的「找不到文件」，看不出原因；
    即使编译出来了，Qt 的 DLL 也不在 exe 旁边（开发期没跑 windeployqt），
    双击会变成「找不到 Qt6WebEngineCore.dll」——同样难懂。

    这个启动器把两种失败都翻译成人话：

      1. exe 存在  → 直接拉起真程序
                     （顺手把 Qt 的 bin 目录塞进子进程的 PATH，省掉 windeployqt）
      2. exe 不存在 → 用原生对话框说明「还没编译」，
                     然后在浏览器里打开 ui/index.html 让你先看界面
                     （前端在无 C++ 后端时会灌演示数据，能完整渲染）

    ui/index.html 是自包含的单文件（含自研离线二维码编码器），
    用系统默认浏览器打开即可，不需要任何服务器。

用法：
    pythonw tools/launch.py           # 快捷方式走这条（无控制台窗口）
    python   tools/launch.py --preview
    python   tools/launch.py --quiet  # 不弹窗，静默按逻辑走
    python   tools/launch.py -- --dev # -- 之后的参数原样传给 MelodySync.exe

退出码：0 = 成功拉起；1 = 出错了（pythonw 下同时会弹窗提示）。
"""

import argparse
import ctypes
import glob
import os
import subprocess
import sys
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EXE_PATH = os.path.join(ROOT, "build", "bin", "MelodySync.exe")
UI_PATH = os.path.join(ROOT, "ui", "index.html")
ICO_PATH = os.path.join(ROOT, "resources", "icons", "melodysync.ico")
QT_HINT = os.path.join(ROOT, "qt_bin.txt")

# 用来判断「这一份 Qt 到底是不是一套可用的运行时」，光看目录名不算数
QT_MARKER = "Qt6Core.dll"

MB_OK = 0x0
MB_ICONERROR = 0x10
MB_ICONINFORMATION = 0x40
MB_ICONWARNING = 0x30
MB_SETFOREGROUND = 0x10000
MB_TOPMOST = 0x40000


def msgbox(text, title="MelodySync", icon=MB_ICONINFORMATION):
    """弹原生消息框。

    用 MessageBoxW 而不是 tkinter：pythonw 下没有控制台可以打印，
    而 tkinter 会拉进一整套 GUI 依赖、启动又慢。user32 的 MessageBoxW
    是零依赖的，还能自己抢到前台（MB_TOPMOST），不会被别的窗口盖住。
    """
    if not hasattr(ctypes, "WinDLL"):          # 非 Windows 平台直接走 stdout
        print("[%s] %s" % (title, text), file=sys.stderr)
        return
    try:
        ctypes.WinDLL("user32").MessageBoxW(
            None, text, title, MB_OK | icon | MB_SETFOREGROUND | MB_TOPMOST)
    except Exception:
        print("[%s] %s" % (title, text), file=sys.stderr)


def _is_qt_bin(path):
    """一个目录要同时满足「是目录」+「里面有 Qt6Core.dll」才算可用 Qt 运行时。

    只看路径像不像会踩坑：C:\\Qt 下面可能只有 Installer 或者别的架构。
    """
    return bool(path) and os.path.isdir(path) and os.path.isfile(
        os.path.join(path, QT_MARKER))


def find_qt_bins():
    """按优先级找出所有可能的 Qt 运行时目录。

    顺序即优先级，前面的先塞进 PATH：
      0. qt_bin.txt 手工指定（一行一个路径，覆盖下面所有自动探测）
      1. 环境变量 QTDIR
      2. <盘>:/Qt/<版本>/msvc*_64/bin   —— 官方在线安装器的默认布局
      3. exe 同级目录 —— 跑过 windeployqt 的情况，DLL 就在旁边
    """
    found = []

    if os.path.isfile(QT_HINT):
        try:
            with open(QT_HINT, "r", encoding="utf-8") as fh:
                for line in fh:
                    line = line.strip().strip('"')
                    if line and not line.startswith("#"):
                        found.append(os.path.normpath(line))
        except OSError:
            pass

    qtdir = os.environ.get("QTDIR", "").strip('"')
    if qtdir:
        # QTDIR 有的工具链指向 Qt 根，有的直接指向 kit 目录，两种都兼容
        found.append(os.path.join(qtdir, "bin"))
        found.append(qtdir)

    for drive in ("C:", "D:"):
        for pat in (os.path.join(drive + os.sep, "Qt", "*", "msvc*_64", "bin"),
                    os.path.join(drive + os.sep, "Qt", "*", "msvc*", "bin")):
            found.extend(glob.glob(pat))

    found.append(os.path.dirname(EXE_PATH))

    # 去重（不区分大小写）后只留下真的装着 Qt6Core.dll 的那些
    seen, out = set(), []
    for p in found:
        p = os.path.normpath(p) if p else ""
        key = os.path.normcase(p)
        if p and key not in seen:
            seen.add(key)
            if _is_qt_bin(p):
                out.append(p)
    return out


def launch_app(extra_args):
    """拉起真正的 MelodySync.exe。返回 (成功?, 补充说明)。"""
    qt_bins = find_qt_bins()
    env = os.environ.copy()
    if qt_bins:
        # 开发期最实用的做法：把 Qt 的 bin 前置到子进程 PATH，
        # 这样不必每次改完代码都跑一遍 windeployqt。
        env["PATH"] = os.pathsep.join(qt_bins) + os.pathsep + env.get("PATH", "")

    try:
        # cwd 设成工程根：程序里的相对路径（--dev 找 ui/index.html）靠它
        subprocess.Popen([EXE_PATH] + extra_args, cwd=ROOT, env=env)
    except OSError as exc:
        return False, str(exc)

    note = ""
    if qt_bins:
        note = "（已把 Qt 运行时目录加入 PATH：%s）" % qt_bins[0]
    return True, note


def open_preview(quiet, qt_bins=None):
    """在系统默认浏览器里打开前端预览。

    对话框里的提示分两种情况，因为这个工程「有 Qt」和「没 Qt」在用户看来
    是两件完全不同的事：前者只差跑一次 build，后者还得先装几个 GB 的 SDK。
    """
    if not os.path.isfile(UI_PATH):
        raise IOError("ui/index.html 不存在：%s" % UI_PATH)

    if not quiet:
        if qt_bins:
            tail = ("本机已检测到 Qt 运行时：\n"
                    "    %s\n"
                    "\n"
                    "下一步：运行工程根目录的 build.bat。\n"
                    "它会先用 vcpkg 拉取并编译第三方依赖\n"
                    "（TagLib / FFmpeg / curl / sqlite3 / libmtp），\n"
                    "首次要二三十分钟以上；之后的增量编译只要几十秒。\n"
                    "\n"
                    "详细步骤见 README.md 第 2、3 节。" % qt_bins[0])
        else:
            tail = ("本机尚未安装 Qt 6.5+（含 WebEngine），\n"
                    "编译前需要先装它 —— 这是唯一的硬性前置条件。")
        msgbox(
            "MelodySync 还没有编译。\n"
            "\n"
            "所以没有 build\\bin\\MelodySync.exe 可以启动。\n"
            "\n"
            "现在先在浏览器里打开界面预览（用演示数据渲染，\n"
            "前端本身是完整的，只是没有 C++ 后端）。\n"
            "\n"
            "想编译成真正的 exe：见 README.md 第 3 节。\n"
            "\n" + tail,
            icon=MB_ICONINFORMATION)

    # os.startfile 走 shell 的「打开」动作，等价于在资源管理器里双击，
    # 因此会使用用户设置的默认浏览器，而不是硬编码某个浏览器。
    os.startfile(UI_PATH)


def main():
    ap = argparse.ArgumentParser(
        description="launch MelodySync (real exe if built, browser preview otherwise)")
    ap.add_argument("--preview", action="store_true",
                    help="跳过 exe 检测，直接在浏览器里打开前端预览")
    ap.add_argument("--quiet", action="store_true",
                    help="不弹对话框（适合脚本调用）")
    ap.add_argument("--dry-run", action="store_true",
                    help="只报告会做什么，不真的启动/打开任何东西")
    ap.add_argument("--exe", default=None,
                    help="覆盖 exe 路径（默认 build/bin/MelodySync.exe）")
    args, extra = ap.parse_known_args()

    global EXE_PATH
    if args.exe:
        EXE_PATH = args.exe if os.path.isabs(args.exe) else os.path.join(ROOT, args.exe)

    # argparse 会把无法识别的参数原样留在 extra 里（含前置的 --），
    # 这正是我们转发给 MelodySync.exe 的东西，不要丢。
    if extra and extra[0] == "--":
        extra = extra[1:]

    if args.dry_run:
        qt_bins = find_qt_bins()
        built = os.path.isfile(EXE_PATH)
        print("project root : %s" % ROOT)
        print("exe          : %s" % EXE_PATH)
        print("exe built    : %s" % ("YES" if built else "no"))
        print("ui preview   : %s (%s)" % (UI_PATH,
                                          "ok" if os.path.isfile(UI_PATH) else "MISSING"))
        print("icon         : %s (%s)" % (ICO_PATH,
                                          "ok" if os.path.isfile(ICO_PATH) else "MISSING"))
        print("qt runtimes  : %s" % (qt_bins or "none found -> Qt SDK not installed"))
        print("forwarded args: %s" % (extra or "(none)"))
        if args.preview or not built:
            print("action       : open browser preview")
        else:
            print("action       : launch %s" % EXE_PATH)
        return 0

    if args.preview:
        open_preview(args.quiet, find_qt_bins())
        return 0

    if os.path.isfile(EXE_PATH):
        ok, note = launch_app(extra)
        if ok:
            return 0
        if not args.quiet:
            msgbox("启动失败：\n%s\n\n%s" % (EXE_PATH, note), icon=MB_ICONERROR)
        return 1

    # 没编译：退到浏览器预览，并把「还缺什么」讲清楚
    try:
        open_preview(args.quiet, find_qt_bins())
    except Exception as exc:
        if not args.quiet:
            msgbox("既没有编译产物，前端文件也打不开：\n\n%s\n\n%s"
                   % (EXE_PATH, exc), icon=MB_ICONERROR)
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception:
        # pythonw 下异常是「静默消失」的，必须自己兜住并显示出来，
        # 否则用户双击快捷方式只会看到「什么都没发生」。
        detail = traceback.format_exc()
        msgbox("启动器出错了：\n\n%s" % detail, icon=MB_ICONERROR)
        sys.exit(1)
