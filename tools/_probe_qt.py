#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时探针：隔离「Qt 安装问题」与「vcpkg toolchain 问题」。

只用 CMAKE_PREFIX_PATH 指向 Qt，完全不加载 vcpkg.cmake。
如果这里能找到 WebEngineWidgets，说明 Qt 安装本身没问题，
锅在 vcpkg toolchain 对 find_package 的包装上。
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run_vcpkg import build_env  # noqa: E402

probe_dir = os.path.join(tempfile.gettempdir(), "qtprobe")
os.makedirs(probe_dir, exist_ok=True)
with open(os.path.join(probe_dir, "CMakeLists.txt"), "w", encoding="utf-8") as fh:
    fh.write(
        "cmake_minimum_required(VERSION 3.21)\n"
        "project(qtprobe LANGUAGES CXX)\n"
        "find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets WebEngineWidgets WebChannel)\n"
        'message(STATUS "PROBE-OK Qt6=${Qt6_VERSION} WebEngineWidgets=${Qt6WebEngineWidgets_VERSION}")\n'
    )

env = build_env()
cmake = r"C:\Users\yfw20\.workbuddy\binaries\python\envs\default\Scripts\cmake.exe"
ninja = (r"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE"
         r"\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe")

cmd = [
    cmake, "-S", probe_dir, "-B", os.path.join(probe_dir, "build"),
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=cl",
    "-DCMAKE_CXX_COMPILER=cl",
    "-DCMAKE_MAKE_PROGRAM=" + ninja,
    "-DCMAKE_PREFIX_PATH=D:/Qt/6.5.3/msvc2019_64",
    "-DQT_DEBUG_FIND_PACKAGE=ON",
    "--debug-find-pkg=Qt6WebEngineWidgets",
]

print("探针目录:", probe_dir)
print("-" * 72)
rc = subprocess.call(cmd, env=env)
print("-" * 72)
print("exit =", rc)
print()
if rc == 0:
    print("=> Qt 安装本身没问题 —— 问题在 vcpkg toolchain 的 find_package 包装上")
else:
    print("=> Qt 安装本身就有问题（与 vcpkg 无关）")
