#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在「已补齐 MSVC / Windows SDK 环境」的条件下运行 vcpkg。

为什么需要这个包装器
====================
vcpkg 在 Windows 上用 Ninja 编译端口时，**不会**自动加载 Visual Studio 的
vcvars 环境。它的工具链 scripts/toolchains/windows.cmake 只设置
CMAKE_RC_FLAGS，并不设置 rc.exe / mt.exe 的位置；而 CMake 生成 Ninja 工程
时是靠 PATH 去 `find_program(CMAKE_RC_COMPILER NAMES rc)` 的。

于是 CMake 的「编译器可用性测试」会这样失败：

    [2/2] cmake -E vs_link_exe --msvc-ver=1951 --rc=rc --mt=CMAKE_MT-NOTFOUND ...
    RC Pass 1: command "rc /fo ...manifest.res ..." failed
    no such file or directory
    -- Check for working C compiler: .../cl.exe - broken

注意 `--rc=rc` —— 它是裸文件名而不是绝对路径，说明 CMake 在 PATH 里
根本没找到它。

本机还有一层额外限制：安全策略禁止 reg.exe 启动，而 vcpkg / CMake 通过
注册表定位 Windows SDK 安装位置的那条路也随之断掉。

所以这里把 vcvars64.bat 本该设置的几个变量手工补齐（INCLUDE / LIB /
PATH / WindowsSdkDir / WindowsSDKVersion），再把环境交给 vcpkg。

用法
====
    python tools/run_vcpkg.py install --triplet x64-windows
    python tools/run_vcpkg.py install --triplet x64-windows --dry-run

不传参数时默认执行 `install --triplet x64-windows`。

如果你的机器上 vcpkg 直接跑就正常（没被沙箱拦 reg.exe），那不需要用它 ——
直接用 `vcpkg install` 即可。这个脚本是无副作用的环境补丁，跑起来等价于
在 Developer Command Prompt 里执行。
"""

import os
import subprocess
import sys

# ---------------------------- 可按需覆盖的路径 ------------------------------
# 工作空间根（MelodySync/tools/run_vcpkg.py 往上三级 = Music/），vcpkg 就放在那里
_VCPKG_DEFAULT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "vcpkg")
VCPKG_ROOT = os.environ.get("VCPKG_ROOT") or _VCPKG_DEFAULT

VS_CANDIDATES = [
    r"C:\Program Files\Microsoft Visual Studio\18\Insiders",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community",
    r"C:\Program Files\Microsoft Visual Studio\2022\Professional",
    r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
    r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools",
]

SDK_ROOT = r"C:\Program Files (x86)\Windows Kits\10"


def _dirs(path):
    try:
        return sorted(d for d in os.listdir(path) if os.path.isdir(os.path.join(path, d)))
    except OSError:
        return []


def pick_vc_tools():
    """挑出 MSVC 工具集目录（形如 .../VC/Tools/MSVC/14.51.36231）。

    可用环境变量 MS_VC_TOOLS_VER 指定具体版本（例如 14.44.35207）。
    这个能力是**必需**的，不是可选优化：

    Qt 6.5.3 的 QtCore/qvarlengtharray.h 里用了 MSVC 的私有扩展
    `stdext::checked_array_iterator`（通过 QT_MAKE_CHECKED_ARRAY_ITERATOR
    宏展开）。而 MSVC 14.51（VS 18 Insiders 自带的最新工具集）已经把它从
    <iterator> 中删除，于是每一个包含 QtCore 头的 .cpp 都会炸：

        qvarlengtharray.h(379): error C3861: "stdext": 找不到标识符

    实测本机各工具集：
        14.34 / 14.42 / 14.44  -> <iterator> 里仍保留 checked_array_iterator
        14.51                  -> 只在 yvals_core.h 留了一句 "STL4043 was ..." 注释

    所以工程固定选 14.44 —— 既够新，又还带着这个扩展。
    """
    want = os.environ.get("MS_VC_TOOLS_VER")
    for vs in VS_CANDIDATES:
        base = os.path.join(vs, "VC", "Tools", "MSVC")
        vers = _dirs(base)
        if not vers:
            continue

        if want and want in vers:
            return vs, os.path.join(base, want), want

        # 版本号按数字段排序，14.51 > 14.44
        def key(v):
            return [int(x) for x in v.split(".") if x.isdigit()]
        best = max(vers, key=key)
        return vs, os.path.join(base, best), best
    return None, None, None


def pick_sdk_version():
    """挑出最新的 Windows SDK 版本目录名（形如 10.0.28000.0）。"""
    vers = [v for v in _dirs(os.path.join(SDK_ROOT, "Include")) if v.startswith("10.")]
    if not vers:
        return None
    def key(v):
        return [int(x) for x in v.split(".") if x.isdigit()]
    return max(vers, key=key)


def build_env():
    vs, vctools, vcver = pick_vc_tools()
    sdkver = pick_sdk_version()

    if not vctools or not sdkver:
        sys.exit("[run_vcpkg] 找不到 MSVC 工具集或 Windows SDK，无法构造编译环境。\n"
                 "            请确认已安装「使用 C++ 的桌面开发」工作负载。")

    env = dict(os.environ)

    def join(*parts):
        return ";".join(parts)

    include = join(
        os.path.join(vctools, "include"),
        os.path.join(SDK_ROOT, "Include", sdkver, "ucrt"),
        os.path.join(SDK_ROOT, "Include", sdkver, "shared"),
        os.path.join(SDK_ROOT, "Include", sdkver, "um"),
        os.path.join(SDK_ROOT, "Include", sdkver, "winrt"),
        os.path.join(SDK_ROOT, "Include", sdkver, "cppwinrt"),
    )
    lib = join(
        os.path.join(vctools, "lib", "x64"),
        os.path.join(SDK_ROOT, "Lib", sdkver, "ucrt", "x64"),
        os.path.join(SDK_ROOT, "Lib", sdkver, "um", "x64"),
    )

    # PATH 顺序照 vcvars64.bat 的习惯：自己的工具在前，系统在后
    front = [
        os.path.join(vctools, "bin", "Hostx64", "x64"),
        os.path.join(SDK_ROOT, "bin", sdkver, "x64"),
        os.path.join(SDK_ROOT, "bin", sdkver, "x86"),
    ]
    env["PATH"] = join(*front, env.get("PATH", ""))

    # 这三组是 CMake / vcpkg 探测 SDK 时会读的
    env["INCLUDE"] = join(include, env.get("INCLUDE", "")) if env.get("INCLUDE") else include
    env["LIB"] = join(lib, env.get("LIB", "")) if env.get("LIB") else lib
    env["WindowsSdkDir"] = SDK_ROOT + "\\"
    env["WindowsSDKVersion"] = sdkver + "\\"
    env["VCToolsInstallDir"] = vctools + "\\"
    env["VSCMD_ARG_TGT_ARCH"] = "x64"

    # vcpkg 默认会清洗环境变量，用这个白名单把上面几个放行给 portfile 的子进程
    keep = ["PATH", "INCLUDE", "LIB", "WindowsSdkDir", "WindowsSDKVersion",
            "VCToolsInstallDir", "VSCMD_ARG_TGT_ARCH"]
    existing = env.get("VCPKG_KEEP_ENV_VARS", "")
    merged = [v for v in existing.split(";") if v] + keep
    seen, ordered = set(), []
    for v in merged:
        if v not in seen:
            seen.add(v)
            ordered.append(v)
    env["VCPKG_KEEP_ENV_VARS"] = ";".join(ordered)

    env.setdefault("VCPKG_DISABLE_METRICS", "1")

    print("[run_vcpkg] MSVC 工具集 : %s (%s)" % (vctools, vcver))
    print("[run_vcpkg] Windows SDK : %s" % sdkver)
    print("[run_vcpkg] rc.exe      : %s" % os.path.join(SDK_ROOT, "bin", sdkver, "x64", "rc.exe"))
    print("[run_vcpkg] VCPKG_ROOT  : %s" % VCPKG_ROOT)
    print()

    return env


def main():
    args = sys.argv[1:]
    if not args:
        args = ["install", "--triplet", "x64-windows"]

    vcpkg_exe = os.path.join(VCPKG_ROOT, "vcpkg.exe")
    if not os.path.isfile(vcpkg_exe):
        sys.exit("[run_vcpkg] 找不到 %s" % vcpkg_exe)

    env = build_env()

    # 在工程根目录执行：vcpkg 的 manifest 模式靠 cwd 找 vcpkg.json
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    print("[run_vcpkg] cwd         : %s" % root)
    print("[run_vcpkg] 执行        : vcpkg %s" % " ".join(args))
    print("-" * 72)

    proc = subprocess.Popen([vcpkg_exe] + args, cwd=root, env=env)
    try:
        return proc.wait()
    except KeyboardInterrupt:
        proc.terminate()
        return 130


if __name__ == "__main__":
    sys.exit(main())
