# ============================================================================
#  MelodySync - CMake 构建脚本
#  目标：生成单个 Windows .exe（Qt6 WebEngine 前端 + C++ 后端）
#
#  典型构建命令（VS 2022 x64）：
#    cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
#          -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
#          -DCMAKE_PREFIX_PATH="C:/Qt/6.6.0/msvc2019_64" ^
#          -DVCPKG_TARGET_TRIPLET=x64-windows
#    cmake --build build --config Release --parallel
# ============================================================================
cmake_minimum_required(VERSION 3.21)

# 云端账户端点（CloudBase 云函数 HTTP 访问地址）：发布机构建时创建
# cmake/secrets.cmake（已 gitignore），内容形如：
#   set(MS_CLOUD_API_BASE "https://<环境ID>.service.tcloudbase.com/ms-account")
# 没有该文件时用空值编译（云端同步停用，纯本地模式，不影响其他功能）。
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/secrets.cmake")
    include(cmake/secrets.cmake)
endif()

project(MelodySync
    VERSION 1.0.0
    DESCRIPTION "MelodySync - 歌单导入 / AI 全网搜 / 无损下载 / 设备同步"
    LANGUAGES CXX)

# ------------------------------- 全局编译选项 -------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 让 VS 生成器把可执行文件统一放到 bin/ 下，方便连同 yt-dlp.exe/ffmpeg.exe 打包
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY        ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG  ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)

if (MSVC)
    # /utf-8 必须开启：本工程所有 .cpp/.h 均以 UTF-8(无 BOM) 保存，
    # 若不告诉 MSVC，它会按系统 ANSI 代码页(中文 Windows = CP936) 解析源码，
    # 于是 QStringLiteral("中文") 里的 UTF-8 字节会被当成 GBK 解读 →
    # 编译期刷 C4819 警告，运行期界面中文全部乱码。加上 /utf-8 后
    # 源码字符集与执行字符集都固定为 UTF-8，中英文系统下行为一致。
    add_compile_options(
        /utf-8              # 源码 + 执行字符集均按 UTF-8
        /MP                 # 多进程编译
        /permissive-        # 严格标准一致性
        /Zc:__cplusplus
        /Zc:preprocessor
        /bigobj             # WebEngine / FFmpeg 头文件模板膨胀，必须
        /wd4996
    )
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS
    )
    # 发布版默认开 LTCG，减小体积
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# ------------------------------- Qt 6 --------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Widgets
    Network
    Sql
    WebEngineWidgets
    WebChannel
)

qt_standard_project_setup()   # 自动处理 AUTOMOC / AUTORCC / AUTOUIC

# ------------------------------- 第三方库 -----------------------------------
find_package(TagLib REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(LIBMTP QUIET libmtp)
endif()

# FFmpeg：优先使用 vcpkg 提供的 CONFIG 包，找不到则退回手工探测
find_package(FFMPEG QUIET COMPONENTS avcodec avformat avutil swresample)
if (NOT FFMPEG_FOUND)
    find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
    foreach (_lib avcodec avformat avutil swresample)
        find_library(FFMPEG_${_lib}_LIBRARY NAMES ${_lib})
    endforeach()
endif()

# ------------------------------- 源码清单 -----------------------------------
set(MS_SOURCES
    src/main.cpp
    src/MainWindow.cpp

    src/core/Types.cpp
    src/core/HttpClient.cpp
    src/core/Async.cpp
    src/data/Database.cpp

    src/bridge/WebBridge.cpp
    src/bridge/LocalAudioServer.cpp

    src/auth/UserManager.cpp
    src/auth/CloudSync.cpp

    src/ai/AiClient.cpp
    src/ai/IntentParser.cpp

    src/search/SearchAggregator.cpp
    src/search/sources/JamendoSource.cpp
    src/search/sources/FmaSource.cpp
    src/search/sources/ArchiveSource.cpp
    src/search/sources/PixabaySource.cpp
    src/search/sources/PreviewSource.cpp
    src/search/sources/WebSource.cpp

    src/playlist/PlaylistParser.cpp
    src/playlist/NeteaseApi.cpp
    src/playlist/QQMusicApi.cpp
    src/playlist/PlatformResolve.cpp

    src/download/DownloadEngine.cpp
    src/download/YtDlpWrapper.cpp

    src/audio/TagWriter.cpp
    src/audio/FFmpegTranscoder.cpp

    src/device/DeviceManager.cpp
    src/device/MtpSync.cpp
    src/device/UsbSync.cpp

    src/settings/Settings.cpp
)

set(MS_HEADERS
    src/MainWindow.h

    src/core/Types.h
    src/core/HttpClient.h
    src/core/Async.h
    src/data/Database.h

    src/bridge/WebBridge.h
    src/bridge/LocalAudioServer.h

    src/auth/UserManager.h
    src/auth/CloudSync.h

    src/ai/AiClient.h
    src/ai/IntentParser.h

    src/search/SearchSource.h
    src/search/SearchAggregator.h
    src/search/sources/JamendoSource.h
    src/search/sources/FmaSource.h
    src/search/sources/ArchiveSource.h
    src/search/sources/PixabaySource.h
    src/search/sources/PreviewSource.h
    src/search/sources/WebSource.h

    src/playlist/PlaylistParser.h
    src/playlist/NeteaseApi.h
    src/playlist/QQMusicApi.h
    src/playlist/PlatformResolve.h

    src/download/DownloadEngine.h
    src/download/YtDlpWrapper.h

    src/audio/TagWriter.h
    src/audio/FFmpegTranscoder.h

    src/device/DeviceManager.h
    src/device/MtpSync.h
    src/device/UsbSync.h

    src/settings/Settings.h
)

# Windows 资源脚本：把 resources/icons/melodysync.ico 链进 PE 的资源节，
# 这样 exe 在资源管理器 / 任务栏 / Alt+Tab 里显示的都是应用图标，
# 而不是系统给的「未知程序」灰方块。
# 刻意单独放一个变量、不塞进 MS_SOURCES —— tools/check_sources.py 是拿
# MS_SOURCES / MS_HEADERS 两个块跟 src/ 下的文件做一对一对账的，
# 掺进一个 resources/ 下的文件会被它误判成「清单里有、磁盘上找不到」。
if (WIN32)
    set(MS_WIN_RESOURCES resources/MelodySync.rc)
endif()

# ------------------------------- 目标 ---------------------------------------
qt_add_executable(MelodySync WIN32 ${MS_SOURCES} ${MS_HEADERS} ${MS_WIN_RESOURCES})

# qrc 资源（把 ui/index.html 打进 exe）
qt_add_resources(MelodySync "melodysync_resources"
    PREFIX "/"
    FILES
        ui/index.html
)

target_include_directories(MelodySync PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(MelodySync PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Sql
    Qt6::WebEngineWidgets
    Qt6::WebChannel

    TagLib::TagLib
    CURL::libcurl
    nlohmann_json::nlohmann_json
    $<IF:$<TARGET_EXISTS:FFMPEG::avcodec>,FFMPEG::avcodec,${FFMPEG_avcodec_LIBRARY}>
    $<IF:$<TARGET_EXISTS:FFMPEG::avformat>,FFMPEG::avformat,${FFMPEG_avformat_LIBRARY}>
    $<IF:$<TARGET_EXISTS:FFMPEG::avutil>,FFMPEG::avutil,${FFMPEG_avutil_LIBRARY}>
    $<IF:$<TARGET_EXISTS:FFMPEG::swresample>,FFMPEG::swresample,${FFMPEG_swresample_LIBRARY}>
    ws2_32
    crypt32
    setupapi
    shell32
    ole32
)

if (LIBMTP_FOUND)
    target_include_directories(MelodySync PRIVATE ${LIBMTP_INCLUDE_DIRS})
    target_link_directories(MelodySync PRIVATE ${LIBMTP_LIBRARY_DIRS})
    target_link_libraries(MelodySync PRIVATE ${LIBMTP_LIBRARIES})
    target_compile_definitions(MelodySync PRIVATE MS_HAVE_LIBMTP=1)
    message(STATUS "MelodySync: libmtp 已启用 -> ${LIBMTP_LIBRARIES}")
else()
    target_compile_definitions(MelodySync PRIVATE MS_HAVE_LIBMTP=0)
    message(WARNING "MelodySync: 未找到 libmtp，MTP 同步将降级为「仅识别设备」模式")
endif()

# 云端账户（CloudBase）端点注入；未定义时 CloudSync 用空串兜底
target_compile_definitions(MelodySync PRIVATE
    MS_CLOUD_API_BASE="${MS_CLOUD_API_BASE}"
)

if (FFMPEG_INCLUDE_DIR)
    target_include_directories(MelodySync PRIVATE ${FFMPEG_INCLUDE_DIR})
endif()

# ------------------------------- 运行时资源拷备 -----------------------------
# 把 UI 用到的外部 exe 拷到输出目录，保证「单个 exe + 两个 sidecar」即可运行
set(MS_RUNTIME_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/bin/yt-dlp.exe"
    "${CMAKE_CURRENT_SOURCE_DIR}/bin/ffmpeg.exe"
)
foreach (_f ${MS_RUNTIME_EXTRA})
    if (EXISTS "${_f}")
        add_custom_command(TARGET MelodySync POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" "$<TARGET_FILE_DIR:MelodySync>")
    endif()
endforeach()

# ------------------------------- 安装规则 -----------------------------------
include(GNUInstallDirs)
install(TARGETS MelodySync RUNTIME DESTINATION .)

message(STATUS "==============================================")
message(STATUS " MelodySync ${PROJECT_VERSION} 配置完成")
message(STATUS "   Qt       : ${Qt6_VERSION}")
message(STATUS "   TagLib   : ${TagLib_VERSION}")
message(STATUS "   输出目录 : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "==============================================")
