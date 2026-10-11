#!/usr/bin/env python3
# ============================================================================
#  android/build.py —— MelodySync 安卓版一键构建（骨架阶段）
#  用法：python android\build.py            # 配置 + 编译 + 出 APK
#        python android\build.py configure  # 只配置
#  工具链布局（国内镜像手动组装，无 Android Studio 依赖）：
#    deps/android/jdk-17.0.2        OpenJDK 17（华为镜像）
#    deps/android/sdk               平台/构建工具/platform-tools（腾讯镜像）
#    deps/android/android-ndk-r25b  NDK r25b（Qt 6.5 官配，腾讯镜像）
#    D:/Qt/6.5.3/android_arm64_v8a  Qt 安卓套件（aqt + 阿里镜像）
#    D:/Qt/6.5.3/msvc2019_64        Qt 宿主工具（qmake/moc 已有）
# ============================================================================
import os
import subprocess
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent          # MelodySync/android
_REPO = _HERE.parent                              # MelodySync/
_DEPS = _REPO.parent / "deps" / "android"         # 工作空间 deps/android

QT_ANDROID = Path(r"D:/Qt/6.5.3/android_arm64_v8a")
QT_HOST = Path(r"D:/Qt/6.5.3/msvc2019_64")
SDK = _DEPS / "sdk"
NDK = _DEPS / "android-ndk-r25b"
JDK = _DEPS / "jdk-17.0.2"
BUILD = _HERE / "build-android"

# 桌面版同款 CMake 3.30.5（工程 venv 里，CMake 4.x 编不过 Qt 6.5）
_CMAKE = Path(r"C:/Users/yfw20/.workbuddy/binaries/python/envs/default/Scripts/cmake.exe")
if not _CMAKE.exists():
    import shutil
    _CMAKE = Path(shutil.which("cmake") or "cmake")

# NDK r25 内置 clang 需要 JDK17 跑 gradle；cmake 只认 ANDROID_NDK_ROOT
ENV = os.environ.copy()
ENV["JAVA_HOME"] = str(JDK)
ENV["ANDROID_SDK_ROOT"] = str(SDK)
ENV["ANDROID_NDK_ROOT"] = str(NDK)
ENV["ANDROID_SDK_HOME"] = str(SDK)
# ninja 在 VS Insiders 自带目录（与桌面 build.py 同源）
_NINJA_DIR = Path(r"C:/Program Files/Microsoft Visual Studio/18/Insiders"
                  r"/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja")
ENV["PATH"] = f"{_NINJA_DIR};{JDK / 'bin'};{NDK};{ENV.get('PATH', '')}"


def _need(paths):
    missing = [str(p) for p in paths if not p.exists()]
    if missing:
        print("[X] 工具链缺失，先跑安装步骤：")
        for m in missing:
            print("    " + m)
        sys.exit(2)


def configure():
    _need([QT_ANDROID / "bin/qmake.bat", QT_HOST / "bin/qmake.exe",
           SDK / "platforms", SDK / "build-tools/33.0.2", NDK / "source.properties",
           JDK / "bin/java.exe"])
    cmd = [
        str(_CMAKE), "-G", "Ninja",
        "-S", str(_HERE), "-B", str(BUILD),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_TOOLCHAIN_FILE=" + str(QT_ANDROID / "lib/cmake/Qt6/qt.toolchain.cmake"),
        "-DQT_HOST_PATH=" + str(QT_HOST),
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-23",
        "-DANDROID_SDK_ROOT=" + str(SDK),
        "-DANDROID_NDK_ROOT=" + str(NDK),
        "-DQT_ANDROID_SDK_ROOT=" + str(SDK),
        "-DQT_ANDROID_NDK_ROOT=" + str(NDK),
        # 桌面版同款约束：UTF-8 无 BOM 源码
        "-DCMAKE_CXX_FLAGS=-DANDROID -std=c++17",
    ]
    print("[1/2] cmake 配置")
    r = subprocess.run(cmd, env=ENV)
    if r.returncode != 0:
        sys.exit(r.returncode)


def build_apk():
    print("[2/2] 编译 + 打 APK（gradle 首跑要下载依赖，几分钟）")
    r = subprocess.run([str(_CMAKE), "--build", str(BUILD), "--target", "apk"],
                       env=ENV, cwd=str(_HERE))
    if r.returncode != 0:
        sys.exit(r.returncode)
    apks = list(BUILD.rglob("*.apk"))
    print()
    print("=" * 60)
    for a in apks:
        print("APK:", a)
    print("=" * 60)


if __name__ == "__main__":
    if "configure" in sys.argv:
        configure()
    else:
        configure()
        build_apk()
