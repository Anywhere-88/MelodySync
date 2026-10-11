// ============================================================================
//  src/device/UsbSync.cpp
//
//  ★ 关键算法：增量同步（文件名 + 大小 + 修改时间）
//  ---------------------------------------------------------------------------
//  为什么不直接"目标存在就跳过"：
//    用户重新下载了一首同名但音质更高的歌（320K → FLAC），
//    如果只比文件名就会永远不同步，播放器里那首歌永远是旧的。
//
//  三级判定（按成本从低到高，任一级不通过就立刻判定"需要重拷"）：
//    ① 文件名：不存在 → 需要拷贝（最便宜，一次 stat）
//    ② 文件大小：不同 → 需要重拷（大小变化是最强的"内容已变"信号）
//    ③ 修改时间：|Δt| > 容差 → 需要重拷
//
//  为什么时间容差是 2 秒而不是 0：
//    FAT32/exFAT 的目录项里，修改时间只精确到 2 秒（5 个 bit 表示秒/2），
//    而 NTFS 是 100 纳秒。把 NTFS 上的文件拷到 FAT32 的 U 盘后，
//    源文件的 mtime 会被"向下取整"到 2 秒边界，两者永远差 0~2 秒。
//    若用精确比较，每次同步都会把所有文件重拷一遍——这是最经典的坑。
//
//  为什么不用哈希（MD5/SHA1）：
//    100 GB 的音乐库算一次全量哈希要几分钟，而 U 盘写入速度本来就是瓶颈；
//    大小+时间戳已经能覆盖 99.9% 的"内容变化"场景，性价比高得多。
//    对音质要求极高的用户，可在设置里打开"同步后校验大小"作为二次保险。
// ============================================================================
#include "device/UsbSync.h"

#include "audio/FFmpegTranscoder.h"
#include "settings/Settings.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QDateTime>

#include <filesystem>

#include <algorithm>

namespace fs = std::filesystem;

namespace ms {

namespace {

const QStringList kAudioExts = {
    QStringLiteral("flac"), QStringLiteral("mp3"), QStringLiteral("m4a"),
    QStringLiteral("aac"),  QStringLiteral("wav"), QStringLiteral("ogg"),
    QStringLiteral("ape"),  QStringLiteral("wma")
};

// 把 QDateTime 转成 time_t 秒
qint64 toEpochSec(const QDateTime& dt) { return dt.isValid() ? dt.toSecsSinceEpoch() : 0; }

} // namespace

QString UsbSync::Plan::toJson() const
{
    QJsonArray arr;
    for (const PlanItem& it : items) {
        QJsonObject o;
        o[QStringLiteral("name")]    = it.fileName;
        o[QStringLiteral("src")]     = it.sourcePath;
        o[QStringLiteral("dst")]     = it.targetPath;
        o[QStringLiteral("size")]    = it.sizeBytes;
        o[QStringLiteral("skip")]    = it.skip;
        o[QStringLiteral("reason")]  = it.skipReason;
        o[QStringLiteral("convert")] = it.needConvert;
        o[QStringLiteral("format")]  = it.convertFormat;
        arr.append(o);
    }
    QJsonObject root;
    root[QStringLiteral("sourceDir")]  = sourceDir;
    root[QStringLiteral("targetRoot")] = targetRoot;
    root[QStringLiteral("total")]      = totalCount;
    root[QStringLiteral("skip")]       = skipCount;
    root[QStringLiteral("copy")]       = copyCount;
    root[QStringLiteral("convert")]    = convertCount;
    root[QStringLiteral("bytes")]      = copyBytes;
    root[QStringLiteral("items")]      = arr;
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

UsbSync::UsbSync(QObject* parent) : QObject(parent) {}

bool UsbSync::isAudioFile(const QString& fileName)
{
    const QString ext = QFileInfo(fileName).suffix().toLower();
    return kAudioExts.contains(ext);
}

// ---------------------------------------------------------------------------
QStringList UsbSync::collectAudioFiles(const QString& dir, const CancelTokenPtr& cancel)
{
    QStringList out;
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) return out;

    QDirIterator it(dir,
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (cancel && cancel->isCanceled()) break;
        const QString path = it.next();
        if (isAudioFile(path)) out << path;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 单条增量判定
// ---------------------------------------------------------------------------
bool UsbSync::compareItem(const PlanItem& it, const Options& opt, QString* reason)
{
    const QFileInfo target(it.targetPath);

    // ① 文件名不存在 → 必须拷
    if (!target.exists()) {
        if (reason) *reason = QStringLiteral("目标不存在");
        return false;
    }

    // ② 大小不同 → 必须重拷
    const qint64 srcSize = QFileInfo(it.sourcePath).size();
    if (target.size() != srcSize) {
        if (reason) {
            *reason = QStringLiteral("大小不同（源 %1 / 目标 %2）")
                          .arg(formatBytes(srcSize), formatBytes(target.size()));
        }
        return false;
    }

    // ③ 修改时间差超容差 → 必须重拷
    const qint64 srcMtime = toEpochSec(QFileInfo(it.sourcePath).lastModified());
    const qint64 dstMtime = toEpochSec(target.lastModified());
    const qint64 diff = qAbs(srcMtime - dstMtime);
    if (diff > opt.mtimeToleranceSec) {
        if (reason) *reason = QStringLiteral("修改时间不同（差 %1 秒）").arg(diff);
        return false;
    }

    if (reason) *reason = QStringLiteral("已存在且一致");
    return true;      // 三项全部通过 → 可以跳过
}

// ---------------------------------------------------------------------------
// 生成计划
// ---------------------------------------------------------------------------
UsbSync::Plan UsbSync::buildPlan(const QString& sourceDir,
                                 const DeviceInfo& device,
                                 const Options& opt,
                                 const CancelTokenPtr& cancel)
{
    Plan plan;
    plan.sourceDir  = sourceDir;
    plan.targetRoot = device.mountPath;

    if (device.mtp) {
        qWarning() << "[UsbSync] buildPlan 被用于 MTP 设备，应改走 MtpSync";
        return plan;
    }
    if (device.mountPath.isEmpty()) { qWarning() << "[UsbSync] 设备没有挂载点"; return plan; }

    const QString targetDir = QDir(device.mountPath).filePath(opt.targetSubdir);
    const QStringList files = collectAudioFiles(sourceDir, cancel);

    plan.items.reserve(files.size());
    for (const QString& src : files) {
        if (cancel && cancel->isCanceled()) break;

        PlanItem it;
        it.sourcePath = src;
        it.fileName   = QFileInfo(src).fileName();
        it.sizeBytes  = QFileInfo(src).size();

        const QString ext = QFileInfo(src).suffix().toLower();

        // 格式适配：设备不吃这个格式 → 目标文件名换成转换后的扩展名
        FFmpegTranscoder::Options conv = FFmpegTranscoder::planFor(device, ext, opt.targetBitrate);
        it.needConvert = opt.convert && (conv.targetFormat != ext);
        it.convertFormat = it.needConvert ? FFmpegTranscoder::targetExtension(conv) : ext;

        const QString outName = it.needConvert
            ? QFileInfo(src).completeBaseName() + QLatin1Char('.') + it.convertFormat
            : it.fileName;
        it.targetPath = targetDir + QLatin1Char('/') + outName;

        // 增量判定
        if (opt.incremental && !it.needConvert)
            it.skip = compareItem(it, opt, &it.skipReason);
        else if (it.needConvert)
            it.skipReason = QStringLiteral("需要转码为 %1").arg(it.convertFormat.toUpper());
        else
            it.skipReason = QStringLiteral("增量同步已关闭");

        if (it.skip) ++plan.skipCount;
        else {
            ++plan.copyCount;
            if (it.needConvert) ++plan.convertCount;
            plan.copyBytes += it.sizeBytes;
        }
        plan.items.append(it);
    }

    plan.totalCount = plan.items.size();
    qInfo() << "[UsbSync] 计划：" << plan.totalCount << "个文件，跳过" << plan.skipCount
            << "，需传输" << plan.copyCount << "（含转码" << plan.convertCount << "）";
    return plan;
}

// ---------------------------------------------------------------------------
// 执行计划
// ---------------------------------------------------------------------------
bool UsbSync::execute(const Plan& plan,
                      const Options& opt,
                      const std::function<void(int, int, int, const QString&)>& onProgress,
                      const CancelTokenPtr& cancel,
                      QString* errorOut)
{
    QDir().mkpath(QDir(plan.targetRoot).filePath(opt.targetSubdir));

    const Settings& cfg = Settings::instance();
    int done = 0;
    int failed = 0;
    const int total = plan.totalCount;

    for (const PlanItem& it : plan.items) {
        if (cancel && cancel->isCanceled()) {
            if (errorOut) *errorOut = QStringLiteral("已取消");
            return false;
        }

        ++done;
        if (it.skip) {
            if (onProgress)
                onProgress(total > 0 ? done * 100 / total : 100, done, total, it.fileName);
            continue;
        }

        QString writtenPath = it.targetPath;

        // ---- 需要转码：先转到临时文件，再整体搬到目标（避免半成品留在 U 盘里）----
        if (it.needConvert && cfg.autoConvertForDevice()) {
            const QString tmp = QDir::temp().filePath(
                QStringLiteral("melodysync_conv_") + it.fileName + QLatin1Char('.')
                + it.convertFormat);

            FFmpegTranscoder::Options conv;
            conv.targetFormat = it.convertFormat;
            conv.bitrateKbps  = opt.targetBitrate;
            conv.sampleRate   = 44100;
            conv.channels     = 2;

            FFmpegTranscoder transcoder;
            const FFmpegTranscoder::Result tr = transcoder.transcode(
                it.sourcePath, tmp, conv,
                [&](double p) {
                    if (onProgress) {
                        // 转码占整体进度的这一格里的 0-50%
                        const int base = total > 0 ? (done - 1) * 100 / total : 0;
                        onProgress(base + static_cast<int>(p * 50 / std::max(1, total)),
                                   done, total, it.fileName + QStringLiteral(" [转码]"));
                    }
                },
                cancel);

            if (!tr.ok) {
                ++failed;
                qWarning() << "[UsbSync] 转码失败:" << it.fileName << tr.error;
                continue;
            }

            // 把临时文件搬到 U 盘（跨设备移动 → 实际是复制+删除）
            const QString finalPath = QDir(plan.targetRoot).filePath(opt.targetSubdir)
                                      + QLatin1Char('/')
                                      + QFileInfo(it.sourcePath).completeBaseName()
                                      + QLatin1Char('.') + it.convertFormat;
            QFile::remove(finalPath);
            if (!QFile::copy(tmp, finalPath)) {
                ++failed;
                qWarning() << "[UsbSync] 转码结果写入设备失败:" << finalPath;
                QFile::remove(tmp);
                continue;
            }
            QFile::remove(tmp);
            writtenPath = finalPath;

            if (onProgress)
                onProgress(total > 0 ? done * 100 / total : 100, done, total, it.fileName);
            continue;
        }

        // ---- 直传：用 std::filesystem 做流式拷贝 ----
        std::error_code ec;
        QFile::remove(writtenPath);       // 覆盖旧版本（QFile::copy 不覆盖已存在文件）
        fs::copy_file(fs::u8path(it.sourcePath.toStdString()),
                      fs::u8path(writtenPath.toStdString()),
                      fs::copy_options::overwrite_existing,
                      ec);
        if (ec) {
            ++failed;
            qWarning() << "[UsbSync] 拷贝失败:" << it.fileName
                       << QString::fromStdString(ec.message());
            continue;
        }

        // ---- 校验（可选）：大小不一致说明 U 盘写坏了，立即报错而不是留个坏文件 ----
        if (opt.verifyAfterCopy) {
            if (QFileInfo(writtenPath).size() != it.sizeBytes) {
                ++failed;
                qWarning() << "[UsbSync] 校验失败（大小不符）:" << it.fileName;
                QFile::remove(writtenPath);
                continue;
            }
        }

        if (onProgress)
            onProgress(total > 0 ? done * 100 / total : 100, done, total, it.fileName);

        QThread::msleep(2);    // 轻微让出 CPU：大批量拷贝时不至于把 UI 完全饿死
    }

    if (failed > 0) {
        if (errorOut) *errorOut = QStringLiteral("有 %1 个文件未能同步").arg(failed);
        return false;
    }
    return true;
}

} // namespace ms
