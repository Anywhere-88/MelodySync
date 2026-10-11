// ============================================================================
//  src/audio/FFmpegTranscoder.cpp
// ============================================================================
#include "audio/FFmpegTranscoder.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <cstring>

namespace ms {

namespace {

void logCallback(void*, int level, const char* fmt, va_list args)
{
    if (level > AV_LOG_WARNING) return;      // 只记警告与错误，避免刷屏
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    qInfo() << "[FFmpeg]" << QString::fromUtf8(buf).trimmed();
}

// 输出格式 → 编码器名
const char* encoderName(const QString& fmt)
{
    if (fmt == QLatin1String("mp3"))  return "libmp3lame";
    if (fmt == QLatin1String("aac"))  return "aac";
    if (fmt == QLatin1String("flac")) return "flac";
    if (fmt == QLatin1String("ogg"))  return "libvorbis";
    return "libmp3lame";
}

// 编码器可能没编译进去（例如精简版 ffmpeg 没有 libmp3lame），逐个降级尝试
const char* fallbackEncoder(const QString& fmt)
{
    if (fmt == QLatin1String("mp3")) return "aac";     // mp3 不行就退到 aac
    if (fmt == QLatin1String("ogg")) return "flac";
    return nullptr;
}

} // namespace

void FFmpegTranscoder::init()
{
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(logCallback);
    qInfo() << "[FFmpeg] 运行库版本:" << av_version_info();
}

// ---------------------------------------------------------------------------
bool FFmpegTranscoder::probe(const QString& filePath, Info* out, QString* errorOut)
{
    AVFormatContext* inCtx = nullptr;
    const QByteArray path = filePath.toUtf8();
    if (avformat_open_input(&inCtx, path.constData(), nullptr, nullptr) < 0) {
        if (errorOut) *errorOut = QStringLiteral("无法打开文件");
        return false;
    }
    if (avformat_find_stream_info(inCtx, nullptr) < 0) {
        avformat_close_input(&inCtx);
        if (errorOut) *errorOut = QStringLiteral("无法读取流信息");
        return false;
    }

    out->durationSec = inCtx->duration > 0 ? static_cast<int>(inCtx->duration / AV_TIME_BASE) : 0;
    out->container   = QString::fromUtf8(inCtx->iformat ? inCtx->iformat->name : "");

    for (unsigned i = 0; i < inCtx->nb_streams; ++i) {
        AVCodecParameters* par = inCtx->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        out->sampleRate = par->sample_rate;
        out->channels   = par->ch_layout.nb_channels;
        const AVCodec* dec = avcodec_find_decoder(par->codec_id);
        out->codec = QString::fromUtf8(dec ? dec->name : avcodec_get_name(par->codec_id));
        break;
    }

    avformat_close_input(&inCtx);
    return true;
}

// ---------------------------------------------------------------------------
FFmpegTranscoder::Result FFmpegTranscoder::transcode(
    const QString& inputPath,
    const QString& outputPath,
    const Options& opt,
    const std::function<void(double)>& onProgress,
    const CancelTokenPtr& cancel)
{
    Result res;
    AVFormatContext* inCtx = nullptr;
    AVFormatContext* outCtx = nullptr;
    AVCodecContext* decCtx = nullptr;
    AVCodecContext* encCtx = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int audioStream = -1;

    auto cleanup = [&]() {
        if (frame)  av_frame_free(&frame);
        if (pkt)    av_packet_free(&pkt);
        if (swr)    swr_free(&swr);
        if (encCtx) avcodec_free_context(&encCtx);
        if (decCtx) avcodec_free_context(&decCtx);
        if (outCtx) {
            if (!(outCtx->oformat->flags & AVFMT_NOFILE) && outCtx->pb)
                avio_closep(&outCtx->pb);
            avformat_free_context(outCtx);
        }
        if (inCtx) avformat_close_input(&inCtx);
    };

    const QByteArray inPath  = inputPath.toUtf8();
    const QByteArray outPath = outputPath.toUtf8();

    if (avformat_open_input(&inCtx, inPath.constData(), nullptr, nullptr) < 0) {
        res.error = QStringLiteral("打开输入失败");
        cleanup();
        return res;
    }
    if (avformat_find_stream_info(inCtx, nullptr) < 0) {
        res.error = QStringLiteral("读取流信息失败");
        cleanup();
        return res;
    }
    audioStream = av_find_best_stream(inCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStream < 0) {
        res.error = QStringLiteral("文件中没有音频流");
        cleanup();
        return res;
    }

    // ---- 解码器 ----
    AVCodecParameters* inPar = inCtx->streams[audioStream]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(inPar->codec_id);
    if (!dec) { res.error = QStringLiteral("找不到解码器"); cleanup(); return res; }
    decCtx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(decCtx, inPar);
    if (avcodec_open2(decCtx, dec, nullptr) < 0) {
        res.error = QStringLiteral("解码器初始化失败");
        cleanup();
        return res;
    }
    res.sourceSampleRate = decCtx->sample_rate;
    res.sourceChannels = decCtx->ch_layout.nb_channels;
    res.sourceCodec = QString::fromUtf8(dec->name);

    // ---- 编码器（含降级）----
    const char* encName = encoderName(opt.targetFormat);
    const AVCodec* enc = avcodec_find_encoder_by_name(encName);
    if (!enc) {
        if (const char* fb = fallbackEncoder(opt.targetFormat))
            enc = avcodec_find_encoder_by_name(fb);
    }
    if (!enc) {
        res.error = QStringLiteral("找不到 %1 编码器（当前 FFmpeg 构建未包含）").arg(opt.targetFormat);
        cleanup();
        return res;
    }

    // ---- 输出上下文 ----
    avformat_alloc_output_context2(&outCtx, nullptr, nullptr, outPath.constData());
    if (!outCtx) { res.error = QStringLiteral("创建输出上下文失败"); cleanup(); return res; }

    encCtx = avcodec_alloc_context3(enc);
    // 采样率：设备友好优先 44100，源本身是 44100/48000 时保持源值
    encCtx->sample_rate = opt.sampleRate > 0 ? opt.sampleRate : decCtx->sample_rate;
    encCtx->bit_rate    = opt.bitrateKbps * 1000;
    encCtx->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
    encCtx->ch_layout.nb_channels = opt.channels > 0 ? opt.channels : decCtx->ch_layout.nb_channels;
    av_channel_layout_default(&encCtx->ch_layout, encCtx->ch_layout.nb_channels);
    // 采样格式：不再查 AVCodec::sample_fmts。
    // 该字段在 FFmpeg 7.1 被标记弃用、8.0 已彻底移除（改用
    // avcodec_get_supported_config），写 enc->sample_fmts 会直接编译失败
    // （error C2039: "sample_fmts": 不是 "AVCodec" 的成员）。
    //
    // 本工程的转码目标只有两个有损编码器：
    //   libmp3lame  —— 接受 FLTP/S16P/S32P
    //   内置 aac    —— 接受 FLTP
    // 两者都支持平面 float，直接指定 FLTP 即可，既省掉一次版本相关的 API
    // 查询，也避免了不同 FFmpeg 版本下取值不一致。
    encCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    if (outCtx->oformat->flags & AVFMT_GLOBALHEADER)
        encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(encCtx, enc, nullptr) < 0) {
        res.error = QStringLiteral("编码器初始化失败");
        cleanup();
        return res;
    }

    AVStream* outStream = avformat_new_stream(outCtx, nullptr);
    avcodec_parameters_from_context(outStream->codecpar, encCtx);
    outStream->time_base = encCtx->time_base;

    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outCtx->pb, outPath.constData(), AVIO_FLAG_WRITE) < 0) {
            res.error = QStringLiteral("无法写入输出文件");
            cleanup();
            return res;
        }
    }
    if (avformat_write_header(outCtx, nullptr) < 0) {
        res.error = QStringLiteral("写文件头失败");
        cleanup();
        return res;
    }

    // ---- 重采样器：解码出来的格式千奇百怪，必须统一到编码器要求 ----
    AVChannelLayout inLayout = decCtx->ch_layout;
    AVChannelLayout outLayout = encCtx->ch_layout;
    if (swr_alloc_set_opts2(&swr,
                            &outLayout, encCtx->sample_fmt, encCtx->sample_rate,
                            &inLayout,  decCtx->sample_fmt, decCtx->sample_rate,
                            0, nullptr) < 0 || swr_init(swr) < 0) {
        res.error = QStringLiteral("重采样器初始化失败");
        cleanup();
        return res;
    }

    pkt   = av_packet_alloc();
    frame = av_frame_alloc();

    const int64_t totalSamples = inCtx->duration > 0
                                     ? av_rescale_q(inCtx->duration, AV_TIME_BASE_Q, encCtx->time_base)
                                     : 0;
    int64_t writtenSamples = 0;
    Throttle throttle(120);          // 进度回调限流，别让 UI 线程被打爆

    auto encodeFrame = [&](AVFrame* f) -> bool {
        if (avcodec_send_frame(encCtx, f) < 0) return false;
        for (;;) {
            AVPacket* op = av_packet_alloc();
            const int r = avcodec_receive_packet(encCtx, op);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) { av_packet_free(&op); break; }
            if (r < 0) { av_packet_free(&op); return false; }
            av_packet_rescale_ts(op, encCtx->time_base, outStream->time_base);
            op->stream_index = outStream->index;
            const int wr = av_interleaved_write_frame(outCtx, op);
            av_packet_free(&op);
            if (wr < 0) return false;
        }
        return true;
    };

    // ---- 主循环：读包 → 解码 → 重采样 → 编码 → 写包 ----
    bool fatal = false;
    while (av_read_frame(inCtx, pkt) >= 0) {
        if (cancel && cancel->isCanceled()) { res.canceled = true; break; }
        if (pkt->stream_index != audioStream) { av_packet_unref(pkt); continue; }

        if (avcodec_send_packet(decCtx, pkt) < 0) { av_packet_unref(pkt); continue; }

        for (;;) {
            const int r = avcodec_receive_frame(decCtx, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) { fatal = true; break; }

            // 重采样到目标格式
            const int outCount = swr_get_out_samples(swr, frame->nb_samples);
            AVFrame* outFrame = av_frame_alloc();
            outFrame->format = encCtx->sample_fmt;
            outFrame->sample_rate = encCtx->sample_rate;
            av_channel_layout_copy(&outFrame->ch_layout, &encCtx->ch_layout);
            outFrame->nb_samples = outCount;
            if (av_frame_get_buffer(outFrame, 0) < 0) { av_frame_free(&outFrame); fatal = true; break; }

            const int converted = swr_convert(swr, outFrame->data, outCount,
                const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            if (converted < 0) { av_frame_free(&outFrame); fatal = true; break; }
            outFrame->nb_samples = converted;
            outFrame->pts = writtenSamples;
            writtenSamples += converted;

            const bool ok = encodeFrame(outFrame);
            av_frame_free(&outFrame);
            if (!ok) { fatal = true; break; }

            if (totalSamples > 0 && onProgress && throttle.ready()) {
                onProgress(qBound(0.0, static_cast<double>(writtenSamples) / totalSamples, 1.0));
            }
        }
        av_packet_unref(pkt);
        if (fatal) break;
    }

    if (!fatal && !res.canceled) {
        // flush 解码器与编码器里残留的帧
        avcodec_send_packet(decCtx, nullptr);
        encodeFrame(nullptr);
        av_write_trailer(outCtx);
        if (onProgress) onProgress(1.0);
        res.ok = true;
    } else if (res.canceled) {
        res.error = QStringLiteral("已取消");
    } else {
        res.error = QStringLiteral("转码过程中出错");
    }

    res.outputPath = outputPath;
    cleanup();

    // 取消或失败时清掉半成品，避免设备里出现 0 字节文件
    if (!res.ok && QFileInfo::exists(outputPath)) QFile::remove(outputPath);
    return res;
}

// ---------------------------------------------------------------------------
// 设备格式适配
// ---------------------------------------------------------------------------
bool FFmpegTranscoder::deviceSupports(const DeviceInfo& dev, const QString& ext)
{
    if (dev.supportedFormats.isEmpty()) return true;      // 信息未知 → 乐观直传
    const QString e = ext.toLower();
    for (const QString& f : dev.supportedFormats)
        if (f.toLower() == e) return true;
    return false;
}

FFmpegTranscoder::Options FFmpegTranscoder::planFor(const DeviceInfo& dev,
                                                    const QString& sourceExt,
                                                    int preferredBitrate)
{
    Options o;
    const QString ext = sourceExt.toLower();

    // 设备支持源格式 → 直传（keepOriginal，不二次编码，省时且不损失音质）
    if (deviceSupports(dev, ext)) {
        o.targetFormat = ext;
        o.keepMetadata = true;
        return o;
    }

    // 设备支持 mp3 → 转 320kbps（几乎所有播放器都吃）
    if (deviceSupports(dev, QStringLiteral("mp3"))) {
        o.targetFormat = QStringLiteral("mp3");
        o.bitrateKbps  = qBound(96, preferredBitrate, 320);
        o.sampleRate   = 44100;      // 老播放器对 48k 支持不稳，统一到 44.1k
        o.channels     = 2;
        return o;
    }

    // 设备支持 aac/m4a → 转 aac 256k（Apple 生态）
    if (deviceSupports(dev, QStringLiteral("m4a"))) {
        o.targetFormat = QStringLiteral("aac");
        o.bitrateKbps  = 256;
        return o;
    }

    // 兜底：转 320 的 mp3，绝大多数设备都能放
    o.targetFormat = QStringLiteral("mp3");
    o.bitrateKbps  = 320;
    return o;
}

QString FFmpegTranscoder::targetExtension(const Options& opt)
{
    if (opt.targetFormat == QLatin1String("aac")) return QStringLiteral("m4a");
    return opt.targetFormat;
}

} // namespace ms
