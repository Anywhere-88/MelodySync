// ============================================================================
//  src/device/MtpSync.cpp
// ============================================================================
#include "device/MtpSync.h"

#include <QDebug>
#include <QFileInfo>

#ifndef MS_HAVE_LIBMTP
#  define MS_HAVE_LIBMTP 0
#endif

#if MS_HAVE_LIBMTP
extern "C" {
#include <libmtp.h>
}
#  include <QCoreApplication>
#  include <QDateTime>
#  include <QMutex>
#endif

namespace ms {

#if MS_HAVE_LIBMTP

namespace {

bool g_libmtpInited = false;
QMutex g_libmtpMutex;          // libmtp 不是线程安全的，所有调用必须串行化

void ensureInit()
{
    QMutexLocker lock(&g_libmtpMutex);
    if (!g_libmtpInited) {
        LIBMTP_Init();
        g_libmtpInited = true;
    }
}

QString friendlyName(uint16_t vendor, uint16_t product, const char* fallback)
{
    // libmtp 内置了一张厂商/型号对照表，优先用它给出的可读名字
    LIBMTP_device_entry_t* entries = nullptr;
    int count = 0;
    if (LIBMTP_Get_Supported_Devices_List(&entries, &count) == 0 && entries) {
        for (int i = 0; i < count; ++i) {
            if (entries[i].vendor_id == vendor && entries[i].product_id == product)
                return QString::fromUtf8(entries[i].vendor) + QLatin1Char(' ')
                     + QString::fromUtf8(entries[i].product);
        }
    }
    if (fallback && *fallback) return QString::fromUtf8(fallback);
    return QStringLiteral("MTP 设备");
}

// 取消令牌在 libmtp 的进度回调里轮询；返回 1 表示中止传输
struct PushCtx
{
    const std::function<void(int, int, int, const QString&)>* onProgress = nullptr;
    CancelTokenPtr cancel;
    int index = 0;
    int total = 0;
    QString fileName;
};

int progressCallback(uint64_t sent, uint64_t total, const char* /*filename*/, void* data)
{
    auto* ctx = static_cast<PushCtx*>(data);
    if (ctx->cancel && ctx->cancel->isCanceled()) return 1;
    if (ctx->onProgress && *ctx->onProgress) {
        const int pct = total > 0 ? static_cast<int>(sent * 100 / total) : 0;
        (*ctx->onProgress)(pct, ctx->index, ctx->total, ctx->fileName);
    }
    return 0;
}

LIBMTP_folder_t* findFolder(LIBMTP_folder_t* folder, const QString& name)
{
    if (!folder) return nullptr;
    for (LIBMTP_folder_t* f = folder; f; f = f->sibling) {
        if (QString::fromUtf8(f->name).compare(name, Qt::CaseInsensitive) == 0) return f;
        if (LIBMTP_folder_t* sub = findFolder(f->child, name)) return sub;
    }
    return nullptr;
}

// 音乐文件 → MTP 的 track 元数据（libmtp 要求提供，否则设备上显示为"未知曲目"）
LIBMTP_track_t* makeTrack(const QFileInfo& fi, uint32_t parentId)
{
    LIBMTP_track_t* t = LIBMTP_new_track_t();
    const QString base = fi.completeBaseName();

    // 文件名约定 "歌手 - 歌名"，据此拆出 artist / title
    QString artist, title;
    const int sep = base.indexOf(QStringLiteral(" - "));
    if (sep > 0) { artist = base.left(sep); title = base.mid(sep + 3); }
    else         { artist = QStringLiteral("Unknown Artist"); title = base; }

    t->title      = strdup(title.toUtf8().constData());
    t->artist     = strdup(artist.toUtf8().constData());
    t->album      = strdup("MelodySync");
    t->genre      = strdup("Unknown");
    t->date       = strdup(QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8().constData());
    t->filename   = strdup(fi.fileName().toUtf8().constData());
    t->filesize   = static_cast<uint32_t>(fi.size());
    t->parent_id  = parentId;
    t->storage_id = 0;
    t->filetype   = fi.suffix().toLower() == QLatin1String("flac")
                        ? LIBMTP_FILETYPE_FLAC : LIBMTP_FILETYPE_MP3;
    return t;
}

} // namespace

QVector<DeviceInfo> MtpSync::enumerateDevices()
{
    QVector<DeviceInfo> out;
    ensureInit();

    QMutexLocker lock(&g_libmtpMutex);
    LIBMTP_raw_device_t* raw = nullptr;
    int count = 0;
    const LIBMTP_error_number_t err = LIBMTP_Detect_Raw_Devices(&raw, &count);
    if (err != LIBMTP_ERROR_NONE || count <= 0) {
        if (raw) free(raw);
        return out;
    }

    for (int i = 0; i < count; ++i) {
        DeviceInfo d;
        d.mtp  = true;
        d.id   = QStringLiteral("mtp:%1:%2:%3")
                     .arg(raw[i].device_entry.vendor_id)
                     .arg(raw[i].device_entry.product_id)
                     .arg(raw[i].bus_location);
        d.name = friendlyName(raw[i].device_entry.vendor_id,
                              raw[i].device_entry.product_id,
                              raw[i].device_entry.product);
        d.type = QStringLiteral("MTP 设备");
        d.icon = d.name.contains(QStringLiteral("Watch"), Qt::CaseInsensitive)
                     || d.name.contains(QStringLiteral("Watch"), Qt::CaseInsensitive)
                     ? QStringLiteral("⌚") : QStringLiteral("🎵");

        // 连一次设备拿容量（连不上就保持 0，UI 侧显示"—"）
        LIBMTP_mtpdevice_t* dev = LIBMTP_Open_Raw_Device_Uncached(&raw[i]);
        if (dev) {
            uint64_t maxCap = 0, freeSpace = 0;
            if (LIBMTP_Get_Storage(dev, LIBMTP_STORAGE_SORTBY_NOTSORTED) == 0) {
                for (LIBMTP_devicestorage_t* s = dev->storage; s; s = s->next) {
                    maxCap   += s->MaxCapacity;
                    freeSpace += s->FreeSpaceInBytes;
                }
            }
            d.totalBytes = static_cast<qint64>(maxCap);
            d.freeBytes  = static_cast<qint64>(freeSpace);
            LIBMTP_Release_Device(dev);
        }

        d.supportedFormats = { QStringLiteral("mp3"), QStringLiteral("flac"),
                               QStringLiteral("m4a") };
        out.append(d);
    }

    if (raw) free(raw);
    qInfo() << "[MTP] 检测到" << out.size() << "个设备";
    return out;
}

QStringList MtpSync::listRemoteFiles(const QString& deviceId, const QString& folder,
                                     QString* errorOut)
{
    Q_UNUSED(deviceId)
    QStringList names;
    ensureInit();

    QMutexLocker lock(&g_libmtpMutex);
    LIBMTP_raw_device_t* raw = nullptr;
    int count = 0;
    if (LIBMTP_Detect_Raw_Devices(&raw, &count) != LIBMTP_ERROR_NONE || count <= 0) {
        if (raw) free(raw);
        if (errorOut) *errorOut = QStringLiteral("未检测到 MTP 设备");
        return names;
    }

    LIBMTP_mtpdevice_t* dev = LIBMTP_Open_Raw_Device_Uncached(&raw[0]);
    free(raw);
    if (!dev) {
        if (errorOut) *errorOut = QStringLiteral("无法打开 MTP 设备（可能被其它程序占用）");
        return names;
    }

    LIBMTP_folder_t* folders = LIBMTP_Get_Folder_List(dev);
    LIBMTP_folder_t* target = findFolder(folders, folder);
    if (target) {
        LIBMTP_file_t* files = LIBMTP_Get_Files_And_Folders(dev, 0, target->folder_id);
        for (LIBMTP_file_t* f = files; f; ) {
            names << QString::fromUtf8(f->filename);
            LIBMTP_file_t* next = f->next;
            LIBMTP_destroy_file_t(f);
            f = next;
        }
    } else if (errorOut) {
        *errorOut = QStringLiteral("设备上没有 %1 目录（将自动创建）").arg(folder);
    }

    if (folders) LIBMTP_destroy_folder_t(folders);
    LIBMTP_Release_Device(dev);
    return names;
}

bool MtpSync::pushFiles(const QString& deviceId,
                        const QStringList& localFiles,
                        const Options& opt,
                        const std::function<void(int, int, int, const QString&)>& onProgress,
                        const CancelTokenPtr& cancel,
                        QString* errorOut)
{
    Q_UNUSED(deviceId)
    ensureInit();

    LIBMTP_raw_device_t* raw = nullptr;
    int count = 0;
    {
        QMutexLocker lock(&g_libmtpMutex);
        if (LIBMTP_Detect_Raw_Devices(&raw, &count) != LIBMTP_ERROR_NONE || count <= 0) {
            if (raw) free(raw);
            if (errorOut) *errorOut = QStringLiteral("未检测到 MTP 设备");
            return false;
        }
    }

    LIBMTP_mtpdevice_t* dev = LIBMTP_Open_Raw_Device_Uncached(&raw[0]);
    free(raw);
    if (!dev) {
        if (errorOut) *errorOut = QStringLiteral("无法打开 MTP 设备");
        return false;
    }

    uint32_t storageId = static_cast<uint32_t>(opt.storageId);
    if (storageId == 0 && dev->storage) storageId = dev->storage->id;

    // 确保目标目录存在
    uint32_t parentId = 0;
    LIBMTP_folder_t* folders = LIBMTP_Get_Folder_List(dev);
    LIBMTP_folder_t* target = findFolder(folders, opt.targetFolder);
    if (target) {
        parentId = target->folder_id;
    } else {
        parentId = LIBMTP_Create_Folder(dev,
                                        const_cast<char*>(opt.targetFolder.toUtf8().constData()),
                                        parentId, storageId);
    }
    if (folders) LIBMTP_destroy_folder_t(folders);

    if (parentId == 0) {
        LIBMTP_Release_Device(dev);
        if (errorOut) *errorOut = QStringLiteral("无法在设备上创建目录 %1").arg(opt.targetFolder);
        return false;
    }

    bool allOk = true;
    for (int i = 0; i < localFiles.size(); ++i) {
        if (cancel && cancel->isCanceled()) {
            if (errorOut) *errorOut = QStringLiteral("已取消");
            allOk = false;
            break;
        }

        const QFileInfo fi(localFiles.at(i));
        if (!fi.exists()) continue;

        // 增量同步：同名文件已存在 → 跳过（大小与时间戳的严格比对在 UsbSync 里做，
        // MTP 侧拿不到远端 mtime，因此只按文件名去重）
        if (opt.existingNames.contains(fi.fileName(), Qt::CaseInsensitive)) continue;

        PushCtx ctx;
        ctx.onProgress = &onProgress;
        ctx.cancel = cancel;
        ctx.index = i;
        ctx.total = localFiles.size();
        ctx.fileName = fi.fileName();

        LIBMTP_track_t* track = makeTrack(fi, parentId);
        const int rc = LIBMTP_Send_Track_From_File(
            dev, fi.absoluteFilePath().toUtf8().constData(), track,
            progressCallback, &ctx);
        LIBMTP_destroy_track_t(track);

        if (rc != 0) {
            qWarning() << "[MTP] 传输失败:" << fi.fileName();
            allOk = false;
        }
    }

    LIBMTP_Release_Device(dev);
    if (!allOk && errorOut && errorOut->isEmpty())
        *errorOut = QStringLiteral("部分文件传输失败");
    return allOk;
}

#else   // ---------------- 未编译 libmtp：功能降级 ----------------

QVector<DeviceInfo> MtpSync::enumerateDevices()
{
    // 不报错、不崩溃，只是返回空。UI 会因此在设备页显示
    // "MTP 支持未编译（需 libmtp）"，用户据此可决定是否重装依赖。
    qInfo() << "[MTP] 本次构建未启用 libmtp，跳过 MTP 设备枚举";
    return {};
}

QStringList MtpSync::listRemoteFiles(const QString&, const QString&, QString* errorOut)
{
    if (errorOut) *errorOut = QStringLiteral("本次构建未启用 libmtp（MTP 同步不可用）");
    return {};
}

bool MtpSync::pushFiles(const QString&, const QStringList&, const Options&,
                        const std::function<void(int, int, int, const QString&)>&,
                        const CancelTokenPtr&, QString* errorOut)
{
    if (errorOut)
        *errorOut = QStringLiteral("本次构建未启用 libmtp，无法同步到 MTP 设备。"
                                  "请在 vcpkg 中安装 libmtp 后重新构建。");
    return false;
}

#endif  // MS_HAVE_LIBMTP

} // namespace ms
