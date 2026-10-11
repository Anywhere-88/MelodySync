# ============================================================================
#  MelodySync Android（骨架阶段）
#  复用桌面版后端核心：Types / IntentParser / AiClient / HttpClient / Settings
#  （全部 QtCore/QtNetwork，无 Windows 头，交叉编译零改动）
# ============================================================================
cmake_minimum_required(VERSION 3.21)

project(MelodySyncAndroid VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Quick)

qt_standard_project_setup()

qt_add_executable(MelodySync
    src/main.cpp
    src/BackendProbe.h
    src/BackendProbe.cpp
    # ---- 后端核心（与桌面版共用同一份源文件，不复制不改名）----
    ../src/core/Types.cpp
    ../src/core/HttpClient.cpp
    ../src/settings/Settings.cpp
    ../src/ai/AiClient.cpp
    ../src/ai/IntentParser.cpp
)

# 桌面版源码内部 include 均相对 src/（如 "ai/IntentParser.h"）
target_include_directories(MelodySync PRIVATE ../src)

# ---- libcurl（vcpkg 交叉编译 arm64-android 静态库，与桌面版同一来源）----
set(MS_CURL_ROOT "D:/OI/AI/AIWork/Music/vcpkg/installed/arm64-android" CACHE PATH
    "vcpkg arm64-android 安装树")
find_package(CURL CONFIG REQUIRED
    HINTS "${MS_CURL_ROOT}/share/curl")
target_link_libraries(MelodySync PRIVATE CURL::libcurl)

target_link_libraries(MelodySync PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Quick
)

# ---- 安卓包属性 ----
set_target_properties(MelodySync PROPERTIES
    QT_ANDROID_PACKAGE_NAME             "com.chitstudio.melodysync"
    QT_ANDROID_APP_NAME                 "MelodySync"
    QT_ANDROID_MIN_SDK_VERSION          "23"
    QT_ANDROID_TARGET_SDK_VERSION       "33"
    QT_ANDROID_SDK_BUILD_TOOLS_REVISION "33.0.2"
)

qt_add_qml_module(MelodySync
    URI MelodySyncApp
    VERSION 1.0
    QML_FILES qml/Main.qml
    NO_RESOURCE_TARGET_PATH
)
