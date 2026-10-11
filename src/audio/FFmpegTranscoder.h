// ============================================================================
//  src/audio/FFmpegTranscoder.h
//  基于 FFmpeg C API 的音频转码器，用于「设备格式适配」：
//  当 U 盘/MP3 播放器不支持 FLAC 时，实时把无损转成 MP3 再拷贝过去。
//
//  为什么用 C API 而不是调用 ffmpeg.exe：
//    - 设备同步的场景下我们已经在跑一个 yt-dlp 子进程，再起一个 exe 容易放大
//      杀软误报概率（子进程链过长）；
//    - C API 可以拿到逐帧进度，做出真实百分比而不是靠解析 stderr 猜；
//    - 转码是 CPU 密集，直接在 worker 线程里跑比进程间 IPC 更省。
//  ffmpeg.exe 仍然保留在 /bin 下，作为设备直传失败时的兜底手段。
// ============================================================================
#pragma once

#include "core/Async.h"
#include "core/Types.h"

#include <QString>

#include <functional>

namespace ms {

class FFmpegTranscoder
{
public:
    struct Options
    {
        QString targetFormat = QStringLiteral("mp3");   // mp3 / aac / flac
        int     bitrateKbps = 320;                      // 有损格式的码率
        int     sampleRate = 0;                         // 0 = 保持源采样率
        int     channels = 0;                           // 0 = 保持源声道数
        bool    keepMetadata = true;
    };

    struct Result
    {
        bool    ok = false;
        bool    canceled = false;
        QString outputPath;
        QString error;
        int     sourceSampleRate = 0;
        int     sourceChannels = 0;
        QString sourceCodec;
    };

    // 进程启动时调用一次（av_register_all 在 FFmpeg 4+ 已废弃，这里只做日志版本号输出）
    static void init();

    // 转码主流程。onProgress(0..1)
    Result transcode(const QString& inputPath,
                     const QString& outputPath,
                     const Options& opt,
                     const std::function<void(double)>& onProgress = {},
                     const CancelTokenPtr& cancel = CancelTokenPtr());

    // 读取音频基本信息（不做解码，只读头）
    struct Info { int durationSec = 0; int sampleRate = 0; int channels = 0; QString codec; QString container; };
    static bool probe(const QString& filePath, Info* out, QString* errorOut = nullptr);

    // -----------------------------------------------------------------------
    // 格式适配决策：设备支持 flac 就直传（保真且省时间），否则转码
    // -----------------------------------------------------------------------
    static bool deviceSupports(const DeviceInfo& dev, const QString& ext);
    static Options planFor(const DeviceInfo& dev, const QString& sourceExt, int preferredBitrate = 320);
    static QString targetExtension(const Options& opt);
};

} // namespace ms
