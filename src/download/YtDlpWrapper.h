// ============================================================================
//  src/download/YtDlpWrapper.h
//  yt-dlp.exe 的 QProcess 封装。
//
//  为什么用 yt-dlp 而不是自己解析各站点的播放地址：
//    - 站点播放地址签名每周都在变，自研解析器的维护成本不可控；
//    - yt-dlp 是成熟的合法工具，默认只走站点公开的媒体通道；
//    - 我们只用它的「搜索」与「下载最高码率原始编码」能力，不做任何绕过。
//
//  可执行文件查找顺序：exe 同级 /bin → exe 同级 → PATH
// ============================================================================
#pragma once

#include "core/Async.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace ms {

class YtDlpWrapper
{
public:
    struct SearchItem
    {
        QString id;
        QString title;
        QString uploader;
        QString url;            // flat-playlist 下常常只是站点内 id，不要当 URL 用
        QString pageUrl;        // 可访问的页面地址（由 prefix 推导），下载阶段交给 yt-dlp
        int     durationSec = 0;
        // 热度信号。flat 模式下站点一般不给（YouTube 的 flat 输出偶有
        // view_count），非 flat 模式（哔哩哔哩必须用）稳定给 —— 实测
        // bilisearch 的 `view_count` / `like_count` 都能拿到。
        qint64  viewCount  = 0;
        qint64  likeCount  = 0;
        QString thumbnail;      // 封面（哔哩哔哩有）
    };

    struct DownloadOptions
    {
        QString outputDir;
        QString outputTemplate = QStringLiteral("%(title)s.%(ext)s");
        QString audioFormat = QStringLiteral("flac");   // flac / mp3 / m4a / best
        QString audioQuality = QStringLiteral("0");     // 0=最高, 或 "320K" / "192K"
        bool    embedThumbnail = true;
        bool    addMetadata = true;
        bool    keepOriginal = false;                   // 保留原始编码不转码
        QStringList extraArgs;
    };

    struct ProgressUpdate
    {
        double percent = 0.0;
        qint64 bytes = 0;
        qint64 totalBytes = 0;
        qint64 speedBps = 0;
        QString stage;          // "下载中" / "转码中" / "处理元数据"
    };

    struct DownloadResult
    {
        bool    ok = false;
        bool    canceled = false;
        QString outputFile;     // 实际落盘文件（从 --print after_move:filepath 拿到）
        QString error;
    };

    YtDlpWrapper();

    static QString executablePath();
    bool available() const { return !executablePath().isEmpty(); }
    QString version() const;

    // 能不能真正跑起来。available() 只看文件在不在 —— 一个被下载截断的
    // yt-dlp.exe 同样"在"，但一启动就报 PyInstaller 解包失败，所有依赖它的
    // 搜索源和下载任务会一起静默失败（这就是之前"下载全失败"的直接原因）。
    // 结果做进程内缓存：探测要起一次进程，不能每次搜索都做。
    // 声明为 static 是为了让"源的可用性"这类 const 查询也能用。
    static bool healthy();                // 可运行
    static QString unavailableReason();   // 不可运行时的人话原因

    // 在后台线程先跑一次健康检查，把结果放进缓存。
    // 启动时调一次，避免首屏 getSources() 在 UI 线程被阻塞几十秒。
    static void prewarm();

    // 搜索：prefix = "ytsearch" / "scsearch" / "bilisearch"（yt-dlp 内置的搜索伪协议）
    //
    // flat = true 时加 `--flat-playlist`：只取搜索页的元数据，快 10 倍以上，
    // 但它**依赖站点在搜索结果里就带齐标题/作者/时长**。
    // YouTube 与 SoundCloud 带，哔哩哔哩不带 —— 实测 bilisearch 的 flat 输出
    // 只有 {id, url, webpage_url}，没有 title/uploader/duration，
    // 于是"标题为空就跳过"的兜底逻辑会把每一条都丢掉，
    // 表现为"搜索成功返回 11KB 数据但解析出 0 条"（这个症状极难定位）。
    // 所以哔哩哔哩必须走 flat=false（代价：5 条约 9 秒，因为是逐条抓页面）。
    bool search(const QString& query, int limit,
                QVector<SearchItem>* out, const QString& prefix,
                QString* errorOut, const CancelTokenPtr& cancel = CancelTokenPtr(),
                bool flat = true);

    // 下载：target 可以是 URL，也可以是 "ytsearch1:关键词"
    DownloadResult download(const QString& target,
                            const DownloadOptions& opt,
                            const std::function<void(const ProgressUpdate&)>& onProgress = {},
                            const CancelTokenPtr& cancel = CancelTokenPtr());

    // 允许外部强制指定路径（便携版场景）
    static void setCustomPath(const QString& path);

private:
    static bool parseProgressLine(const QString& line, ProgressUpdate* out);

    // 把 yt-dlp 的原始报错翻译成一句能给用户看的话。
    // 直接甩"yt-dlp 退出码 1"等于没说 —— 用户根本无从下手。
    static QString humanizeError(const QString& rawStdErr, int exitCode);

    QString m_exe;
};

} // namespace ms
