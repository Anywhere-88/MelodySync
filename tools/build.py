#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 注意这一行开头的 r —— 本文档字符串里满是 Windows 路径（build\bin\...），
# 用普通字符串会让 \b、\M 被当成转义序列（\M 非法，Python 会发 SyntaxWarning，
# \b 更是会静默变成退格字符，让打印出来的说明变成乱码）。加 r 走原始字符串。
r"""构建 MelodySync —— Ninja 生成器 + 已补齐的 MSVC / Windows SDK 环境。

为什么不用 Visual Studio 生成器
==============================
本机装的是 VS 18 Insiders。CMake 4.3.1 虽然认识 "Visual Studio 18 2026"
这个生成器名，配置时也能选出正确的 Windows SDK 版本，但在 project() 阶段报：

    -- The CXX compiler identification is unknown
    CMake Error at CMakeLists.txt:14 (project):
      No CMAKE_CXX_COMPILER could be found.

也就是生成器找到了 VS 安装，却定位不到 MSVC 的 cl.exe。
而同一台机器上 vcpkg 用 Ninja + cl.exe 编译 11 个依赖包完全正常 ——
说明工具链本身没问题，卡点只在 VS 生成器对 Insiders 实例的探测上。

所以这里改走 Ninja：由脚本自己把 vcvars64 该设的环境补齐（复用
run_vcpkg.py 里的 build_env），再把 cl.exe 显式交给 CMake。
构建链上只剩「一条路」，不再依赖 CMake 去猜 VS 装在哪。

用法
====
    python tools/build.py              # 配置 + 编译 Release → build/bin/
    python tools/build.py --debug      # 配置 + 编译 Debug   → build-debug/bin/
    python tools/build.py --configure  # 只配置，不编译
    python tools/build.py --clean      # 删掉构建目录后从头来（换了工具链必须这么干）
    python tools/build.py --run        # 编译完以 --dev 模式启动

之所以 Debug 用独立目录：Ninja 是**单配置**生成器，Debug / Release 挤在同一个
build/ 里会互相覆盖 CMakeCache 和产物。Release 仍固定在 build/，因为 README、
桌面快捷方式、自检脚本都指向 `build\bin\MelodySync.exe`。
"""

import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from run_vcpkg import build_env, VCPKG_ROOT  # noqa: E402

ROOT = os.path.dirname(HERE)

# 构建目录按配置区分。**不能共用**：Ninja 是单配置生成器，Debug / Release
# 混在同一个 build/ 里会互相覆盖 CMakeCache 与产物。
# 约定：Release 仍是 build/（README 与快捷方式都指向它），Debug 走 build-debug/。
BUILD_DIR_RELEASE = os.path.join(ROOT, "build")
BUILD_DIR_DEBUG = os.path.join(ROOT, "build-debug")

# Qt 与手工放置的 ffmpeg 前缀。ffmpeg 走预编译包（见 README §2.6），
# CMakeLists 里有 find_path/find_library 的手工探测回退路径，能直接接上。
QT_DIR = os.environ.get("QTDIR") or r"D:\Qt\6.5.3\msvc2019_64"
FFMPEG_DIR = os.environ.get("MS_FFMPEG_DIR") or os.path.join(os.path.dirname(ROOT), "deps", "ffmpeg-n9.0-latest-win64-gpl-shared-9.0")


def find_tool(name, candidates):
    for c in candidates:
        if os.path.isfile(c):
            return c
    found = shutil.which(name)
    if found:
        return found
    return None


def locate_cmake_ninja():
    """CMake / Ninja：优先用 vcpkg 下载的那套（已验证可用），其次 VS 自带的。"""
    vcpkg_tools = os.path.join(VCPKG_ROOT, "downloads", "tools")
    cmake = find_tool("cmake", [
        # 首选 CMake 3.30（pip 装到本工程的 venv 里），原因很重要：
        #
        # Qt 6.5.3 的 CMake 包是 CMake 3.x 时代的产物。用 CMake 4.x
        # （无论 VS 自带的 4.3.1 还是 vcpkg 下载的 4.4.3）配置时会在
        #     find_package(Qt6 ... COMPONENTS WebEngineWidgets)
        # 处失败，报：
        #     Failed to find required Qt component "WebEngineWidgets".
        #     Expected Config file at "...Qt6WebEngineWidgetsConfig.cmake" exists
        # 非常误导 —— 那个 Config 文件确实存在，它依赖的 Quick /
        # QuickWidgets / PrintSupport / WebEngineCore 也全都在。
        # 换成 CMake 3.30 后同一份工程直接配置通过。
        #
        # 安装：pip install "cmake==3.30.5"
        os.environ.get("MS_CMAKE") or "",
        r"C:\Users\yfw20\.workbuddy\binaries\python\envs\default\Scripts\cmake.exe",
        os.path.join(vcpkg_tools, "cmake-4.4.3-windows",
                     "cmake-4.4.3-windows-x86_64", "bin", "cmake.exe"),
        r"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE"
        r"\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    ])
    ninja = find_tool("ninja", [
        r"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE"
        r"\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    ])
    return cmake, ninja


def main():
    args = sys.argv[1:]

    is_debug = "--debug" in args
    build_type = "Debug" if is_debug else "Release"
    build_dir = BUILD_DIR_DEBUG if is_debug else BUILD_DIR_RELEASE

    if "--clean" in args and os.path.isdir(build_dir):
        print("[build] 删除 %s" % build_dir)
        shutil.rmtree(build_dir, ignore_errors=True)

    cmake, ninja = locate_cmake_ninja()
    if not cmake:
        sys.exit("[build] 找不到 cmake。请安装 CMake 或设置 PATH。")
    if not ninja:
        sys.exit("[build] 找不到 ninja。VS 自带的位于 "
                 r"Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe")

    if not os.path.isfile(os.path.join(QT_DIR, "lib", "cmake", "Qt6", "Qt6Config.cmake")):
        sys.exit("[build] QTDIR 下没有 Qt6Config.cmake：%s" % QT_DIR)
    if not os.path.isfile(os.path.join(FFMPEG_DIR, "include", "libavcodec", "avcodec.h")):
        sys.exit("[build] FFMPEG_DIR 下没有 libavcodec/avcodec.h：%s" % FFMPEG_DIR)

    # 固定使用 MSVC 14.44 工具集。原因见 run_vcpkg.pick_vc_tools 的注释：
    # 14.51 已删除 stdext::checked_array_iterator，而 Qt 6.5.3 依赖它。
    os.environ.setdefault("MS_VC_TOOLS_VER", "14.44.35207")

    env = build_env()           # 补齐 INCLUDE / LIB / PATH / WindowsSDKVersion
    env["PATH"] = os.path.dirname(ninja) + os.pathsep + env.get("PATH", "")
    env["VCPKG_ROOT"] = VCPKG_ROOT

    print("[build] 配置        : %s" % build_type)
    print("[build] cmake       : %s" % cmake)
    print("[build] ninja       : %s" % ninja)
    print("[build] Qt          : %s" % QT_DIR)
    print("[build] FFmpeg      : %s" % FFMPEG_DIR)
    print("[build] 构建目录     : %s" % build_dir)
    print("-" * 72)

    if not os.path.isfile(os.path.join(build_dir, "build.ninja")):
        cmd = [
            cmake, "-S", ROOT, "-B", build_dir,
            "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=" + build_type,
            "-DCMAKE_C_COMPILER=cl",
            "-DCMAKE_CXX_COMPILER=cl",
            "-DCMAKE_MAKE_PROGRAM=" + ninja,
            "-DCMAKE_TOOLCHAIN_FILE=" +
                os.path.join(VCPKG_ROOT, "scripts", "buildsystems", "vcpkg.cmake").replace("\\", "/"),
            "-DCMAKE_PREFIX_PATH=%s;%s" % (QT_DIR.replace("\\", "/"), FFMPEG_DIR.replace("\\", "/")),
            "-DVCPKG_TARGET_TRIPLET=x64-windows",
            "-DVCPKG_MANIFEST_INSTALL=OFF",     # 依赖已装好，别再触发一次 vcpkg
            # 关键：一旦把 VCPKG_MANIFEST_INSTALL 关掉，vcpkg.cmake 就不再认为
            # 这是 manifest 工程，于是默认去 ${VCPKG_ROOT}/installed 找依赖
            # （我们那个目录是空的），明明 ./vcpkg_installed/ 里齐得很。
            # 必须显式把安装目录指回来，否则 find_package(TagLib REQUIRED)
            # 会报 "Could not find a package configuration file provided by TagLib"。
            "-DVCPKG_INSTALLED_DIR=" + os.path.join(ROOT, "vcpkg_installed").replace("\\", "/"),
        ]
        print("[build] 配置中 ...")
        t0 = time.time()
        rc = subprocess.call(cmd, cwd=ROOT, env=env)
        print("[build] 配置完成 %.1fs (exit=%d)" % (time.time() - t0, rc))
        if rc != 0:
            return rc
    else:
        print("[build] 复用已有 %s（要重配请加 --clean）" % build_dir)

    if "--configure" in args:
        return 0

    print("[build] 编译中 ...")
    t0 = time.time()
    rc = subprocess.call([cmake, "--build", build_dir, "--parallel"], cwd=ROOT, env=env)
    print("[build] 编译结束 %.1fs (exit=%d)" % (time.time() - t0, rc))
    if rc != 0:
        # MSVC 链接器打不开输出文件时给的是一个"裸"退出码（Windows 上会显示成
        # 4294967295 这种），看不出所以然。最常见的原因就是程序还开着占用了 exe。
        print()
        print("[build] 编译失败（exit=%d）。" % rc)
        print("        如果报的是 LNK1104 / 无法打开 …MelodySync.exe：")
        print("        程序还在运行、exe 被占用 —— 先关掉 MelodySync 再重来。")
        print("        其他错误往上翻，C#### / LNK#### 那几行才是真因。")
        return rc

    exe = os.path.join(build_dir, "bin", "MelodySync.exe")
    if os.path.isfile(exe):
        print("[build] 产物: %s (%.1f MB)" % (exe, os.path.getsize(exe) / 2**20))
    else:
        print("[build] 警告: 没找到 %s" % exe)

    if "--run" in args and os.path.isfile(exe):
        print("[build] 以 --dev 模式启动 ...")
        subprocess.Popen([exe, "--dev"], cwd=os.path.join(build_dir, "bin"), env=env)
    return 0


if __name__ == "__main__":
    sys.exit(main())
