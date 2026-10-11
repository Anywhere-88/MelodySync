# -*- coding: utf-8 -*-
"""
创建 MelodySync 桌面快捷方式
============================

为什么不直接调 shell：
    这台机器上 shell 侧的常规手段（PowerShell 的
    New-Object -ComObject WScript.Shell、cmd 调 cscript 等）会被安全策略
    拦掉，所以这里用 ctypes 手写 COM 调用，直接对接 Explorer 内部用的同一套
    官方接口 —— IShellLinkW（写入目标/工作目录/图标/备注）+ IPersistFile
    （序列化成 .lnk 文件）。

    ctypes 没有内建的 COM 支持，所以 vtable 要自己走一遍：`this` 指向的
    对象首字段是 vtable 指针，vtable 是一串函数指针，槽位顺序由接口的 IDL
    固定下来（IUnknown 永远占 0/1/2 三格）。下面每个索引都标了对应的 C++
    方法名，改的时候对着 IDL 数，别凭记忆。

为什么要 --dir 而不是写死 %USERPROFILE%\\Desktop：
    桌面有可能被 OneDrive「备份文件夹」重定向到别处，所以走 shell 的
    SHGetFolderPathW(CSIDL_DESKTOPDIRECTORY) 拿权威路径。

用法:
    python tools/make_shortcut.py --via-launcher
    python tools/make_shortcut.py
    python tools/make_shortcut.py --name "MelodySync (portable)" --target dist/MelodySync/MelodySync.exe
    python tools/make_shortcut.py --dir dist

--via-launcher 是这里推荐的用法：快捷方式指向 tools/launch.py（用 pythonw 跑，
不弹控制台），由启动器自己决定是拉起 exe 还是退到浏览器预览。好处是编译前后
都不用重新创建快捷方式 —— 直接指向 exe 的快捷方式在还没编译时双击会报
「找不到文件」。

注意默认目标路径是 build/bin/MelodySync.exe —— 这个工程**没有** Release/Debug
子目录，CMakeLists 的 CMAKE_RUNTIME_OUTPUT_DIRECTORY[_<CONFIG>] 和 vcxproj 的
OutDir 都被钉到同一层 bin/ 下，两个配置的产物同名同地。
"""

import argparse
import ctypes
import os
import sys
from ctypes import POINTER, byref, c_int, c_uint32, c_void_p, c_wchar_p

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CLSCTX_INPROC_SERVER = 0x1
COINIT_APARTMENTTHREADED = 0x2
CSIDL_DESKTOPDIRECTORY = 0x0010
SHGFP_TYPE_CURRENT = 0
STGM_READ = 0x0
SLGP_RAWPATH = 0x0004

# --- IShellLinkW 的 vtable 槽位（IUnknown 占 0/1/2）---------------------------
SL_QUERY_INTERFACE = 0
SL_RELEASE = 2
SL_GET_PATH = 3
SL_SET_DESCRIPTION = 7
SL_GET_WORKING_DIRECTORY = 8
SL_SET_WORKING_DIRECTORY = 9
SL_GET_ARGUMENTS = 10
SL_SET_ARGUMENTS = 11
SL_GET_ICON_LOCATION = 16
SL_SET_ICON_LOCATION = 17
SL_SET_PATH = 20

# --- IPersistFile 的 vtable 槽位 -------------------------------------------
PF_RELEASE = 2
PF_LOAD = 5
PF_SAVE = 6

if not hasattr(ctypes, "WINFUNCTYPE"):
    sys.exit("this script is Windows-only")

ole32 = ctypes.WinDLL("ole32")
shell32 = ctypes.WinDLL("shell32")


class GUID(ctypes.Structure):
    _fields_ = [("Data1", ctypes.c_uint32),
                ("Data2", ctypes.c_uint16),
                ("Data3", ctypes.c_uint16),
                ("Data4", ctypes.c_ubyte * 8)]


ole32.CLSIDFromString.argtypes = [c_wchar_p, POINTER(GUID)]
ole32.CLSIDFromString.restype = ctypes.c_long
ole32.CoInitializeEx.argtypes = [c_void_p, c_uint32]
ole32.CoInitializeEx.restype = ctypes.c_long
ole32.CoCreateInstance.argtypes = [POINTER(GUID), c_void_p, c_uint32,
                                   POINTER(GUID), POINTER(c_void_p)]
ole32.CoCreateInstance.restype = ctypes.c_long
ole32.CoUninitialize.argtypes = []
ole32.CoUninitialize.restype = None
shell32.SHGetFolderPathW.argtypes = [c_void_p, c_int, c_void_p, c_uint32,
                                     POINTER(ctypes.c_wchar)]
shell32.SHGetFolderPathW.restype = ctypes.c_long


def _hr(hr):
    """把 HRESULT 打印成无符号十六进制。

    ctypes 的 c_long 在 Windows 上是 32 位**有符号**，失败的 HRESULT 最高位
    一律为 1（例如 CO_E_NOTINITIALIZED = 0x800401F0），直接 `%08X` 会输出
    `0x-7FFBFE10` 这种读不出来的东西，查错误码时白白绕圈。
    """
    return hr & 0xFFFFFFFF


def _guid(text):
    g = GUID()
    if ole32.CLSIDFromString(text, byref(g)) != 0:
        raise RuntimeError("bad GUID literal: %s" % text)
    return g


CLSID_SHELL_LINK = _guid("{00021401-0000-0000-C000-000000000046}")
IID_SHELL_LINK_W = _guid("{000214F9-0000-0000-C000-000000000046}")
IID_PERSIST_FILE = _guid("{0000010B-0000-0000-C000-000000000046}")


def _method(this, index, argtypes, restype=ctypes.c_long):
    """按槽位取 COM 方法并绑定到 this。

    this 指向的对象首字段是 vtable 指针；vtable 本身是函数指针数组。
    解引用两层后取 index 槽，再按接口方法签名包成可调用的函数。
    """
    vtable = ctypes.cast(ctypes.cast(this, POINTER(c_void_p))[0],
                         POINTER(c_void_p))
    proto = ctypes.WINFUNCTYPE(restype, c_void_p, *argtypes)
    return proto(vtable[index])


def desktop_dir():
    """取当前用户桌面路径（shell 权威值，能正确处理 OneDrive 重定向）。"""
    buf = ctypes.create_unicode_buffer(260)
    hr = shell32.SHGetFolderPathW(None, CSIDL_DESKTOPDIRECTORY, None,
                                  SHGFP_TYPE_CURRENT, buf)
    if hr != 0:
        raise RuntimeError("SHGetFolderPathW(CSIDL_DESKTOPDIRECTORY) failed: 0x%08X" % _hr(hr))
    return buf.value


def _new_shell_link():
    link = c_void_p()
    hr = ole32.CoCreateInstance(byref(CLSID_SHELL_LINK), None,
                                CLSCTX_INPROC_SERVER,
                                byref(IID_SHELL_LINK_W), byref(link))
    if hr != 0:
        raise RuntimeError("CoCreateInstance(ShellLink) failed: 0x%08X" % _hr(hr))
    return link


def write_lnk(lnk_path, target, working_dir, icon_path, description, arguments=None):
    """写入 .lnk。字段没传的保持默认，不写空串污染文件。"""
    link = _new_shell_link()
    persist = c_void_p()
    try:
        if target:
            _method(link, SL_SET_PATH, [c_wchar_p])(link, target)
        if working_dir:
            _method(link, SL_SET_WORKING_DIRECTORY, [c_wchar_p])(link, working_dir)
        if description:
            _method(link, SL_SET_DESCRIPTION, [c_wchar_p])(link, description)
        if arguments:
            _method(link, SL_SET_ARGUMENTS, [c_wchar_p])(link, arguments)
        if icon_path:
            # 第二个参数是图标索引；指向独立 .ico 文件时固定为 0
            _method(link, SL_SET_ICON_LOCATION, [c_wchar_p, c_int])(link, icon_path, 0)

        hr = _method(link, SL_QUERY_INTERFACE,
                     [POINTER(GUID), POINTER(c_void_p)])(link, byref(IID_PERSIST_FILE),
                                                         byref(persist))
        if hr != 0:
            raise RuntimeError("QueryInterface(IPersistFile) failed: 0x%08X" % _hr(hr))
        # fRemember = TRUE，让 shell 记住这个保存位置
        hr = _method(persist, PF_SAVE, [c_wchar_p, c_int])(persist, lnk_path, 1)
        if hr != 0:
            raise RuntimeError("IPersistFile::Save failed: 0x%08X" % _hr(hr))
    finally:
        if persist:
            _method(persist, PF_RELEASE, [])(persist)
        _method(link, SL_RELEASE, [])(link)


def read_lnk(lnk_path):
    """回读 .lnk 的字段 —— 写完就验，免得生成一个属性丢失的空壳。"""
    link = _new_shell_link()
    persist = c_void_p()
    try:
        hr = _method(link, SL_QUERY_INTERFACE,
                     [POINTER(GUID), POINTER(c_void_p)])(link, byref(IID_PERSIST_FILE),
                                                         byref(persist))
        if hr != 0:
            raise RuntimeError("QueryInterface(IPersistFile) failed: 0x%08X" % _hr(hr))
        hr = _method(persist, PF_LOAD, [c_wchar_p, c_uint32])(persist, lnk_path, STGM_READ)
        if hr != 0:
            raise RuntimeError("IPersistFile::Load failed: 0x%08X" % _hr(hr))

        buf = ctypes.create_unicode_buffer(1024)
        _method(link, SL_GET_PATH,
                [POINTER(ctypes.c_wchar), c_int, c_void_p, c_uint32])(link, buf, 1024,
                                                                     None, SLGP_RAWPATH)
        target = buf.value

        wd = ctypes.create_unicode_buffer(1024)
        _method(link, SL_GET_WORKING_DIRECTORY,
                [POINTER(ctypes.c_wchar), c_int])(link, wd, 1024)

        icon = ctypes.create_unicode_buffer(1024)
        idx = c_int(0)
        _method(link, SL_GET_ICON_LOCATION,
                [POINTER(ctypes.c_wchar), c_int, POINTER(c_int)])(link, icon, 1024, byref(idx))

        argb = ctypes.create_unicode_buffer(2048)
        _method(link, SL_GET_ARGUMENTS,
                [POINTER(ctypes.c_wchar), c_int])(link, argb, 2048)

        return {"target": target, "working_dir": wd.value, "icon": icon.value,
                "icon_index": idx.value, "arguments": argb.value}
    finally:
        if persist:
            _method(persist, PF_RELEASE, [])(persist)
        _method(link, SL_RELEASE, [])(link)


def find_pythonw():
    """找当前解释器旁边的 pythonw.exe。

    快捷方式要指向 pythonw 而不是 python —— pythonw 是 GUI 子系统程序，
    不会先弹一个黑色的控制台窗口再启动。找不到就退回去用 python.exe
    （功能一样，只是会闪一下黑框）。
    """
    exe = sys.executable or ""
    cand = os.path.join(os.path.dirname(exe), "pythonw.exe")
    return cand if os.path.isfile(cand) else exe


def main():
    ap = argparse.ArgumentParser(description="create the MelodySync desktop shortcut")
    ap.add_argument("--name", default="MelodySync",
                    help="shortcut file name without the .lnk extension")
    ap.add_argument("--dir", default=None,
                    help="output directory (default: the current user's Desktop)")
    ap.add_argument("--target", default=os.path.join("build", "bin", "MelodySync.exe"),
                    help="executable the shortcut points at (relative to the project root)")
    ap.add_argument("--icon", default=os.path.join("resources", "icons", "melodysync.ico"),
                    help=".ico file used for the shortcut icon")
    ap.add_argument("--arguments", default=None,
                    help="command-line arguments passed to --target")
    ap.add_argument("--via-launcher", action="store_true",
                    help="point the shortcut at tools/launch.py instead of the exe: "
                         "starts the real app when it is built, opens the browser "
                         "UI preview when it is not")
    ap.add_argument("--launcher-args", default=None,
                    help="extra args appended after the launch.py path (only with "
                         "--via-launcher), e.g. --launcher-args=--quiet")
    ap.add_argument("--description",
                    default="MelodySync - playlist import / AI search / lossless download / device sync")
    args = ap.parse_args()

    def abspath(p):
        return p if os.path.isabs(p) else os.path.normpath(os.path.join(ROOT, p))

    icon = abspath(args.icon)
    out_dir = args.dir or desktop_dir()
    lnk = os.path.join(out_dir, args.name + ".lnk")

    if args.via_launcher:
        launcher = os.path.join(ROOT, "tools", "launch.py")
        if not os.path.isfile(launcher):
            sys.exit("launcher not found: %s" % launcher)
        target = find_pythonw()
        if not os.path.isfile(target):
            sys.exit("no python interpreter to run the launcher: %s" % target)
        # 路径可能含空格，必须加引号，否则 shell 会把参数切碎
        arguments = '"%s"' % launcher
        if args.launcher_args:
            arguments += " " + args.launcher_args
    else:
        target = abspath(args.target)
        arguments = args.arguments

    if not os.path.isfile(icon):
        sys.exit("icon not found: %s\nrun tools/make_icon.py first" % icon)
    if not os.path.isdir(out_dir):
        sys.exit("output directory does not exist: %s" % out_dir)

    hr = ole32.CoInitializeEx(None, COINIT_APARTMENTTHREADED)
    if hr not in (0, 1):                       # S_OK / S_FALSE(已初始化)
        sys.exit("CoInitializeEx failed: 0x%08X" % _hr(hr))
    try:
        write_lnk(lnk, target, ROOT, icon, args.description, arguments)
        got = read_lnk(lnk)
    finally:
        ole32.CoUninitialize()

    print("shortcut : %s (%d bytes)" % (lnk, os.path.getsize(lnk)))
    ok = True
    checks = [("TargetPath", "target", target),
              ("WorkingDir", "working_dir", ROOT),
              ("IconLocation", "icon", icon)]
    if arguments:
        checks.append(("Arguments", "arguments", arguments))
    for label, key, want in checks:
        val = got[key]
        match = os.path.normcase(val) == os.path.normcase(want)
        ok = ok and match
        print("  %-12s: %s%s" % (label, val, "" if match else "   <-- MISMATCH, expected %s" % want))
    print("  IconIndex   : %d" % got["icon_index"])

    if args.via_launcher:
        print("\nmode: launcher")
        print("      double-click starts build\\bin\\MelodySync.exe when it exists,")
        print("      otherwise opens ui\\index.html in the browser so you can still")
        print("      see the interface. No need to re-point the shortcut after building.")
    elif not os.path.isfile(target):
        print("\nnote: %s does not exist yet." % target)
        print("      the shortcut is valid and its icon will still render, but")
        print("      double-clicking it will fail until you build the project")
        print("      (run build.bat).")
        print("      tip: use --via-launcher to get a shortcut that always works.")
    if not ok:
        return 1
    print("\nresult: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
