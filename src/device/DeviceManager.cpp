// ============================================================================
//  src/device/DeviceManager.cpp
// ============================================================================
#include "device/DeviceManager.h"

#include "device/MtpSync.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dbt.h>
#  include <fileapi.h>
#endif

namespace ms {

DeviceManager::DeviceManager(QObject* parent) : QObject(parent)
{
    // 去抖 700ms：一次插拔会产生多条 WM_DEVICECHANGE，合并成一次枚举
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(700);
    connect(&m_debounce, &QTimer::timeout, this, &DeviceManager::doRefresh);
}

DeviceManager::~DeviceManager() { stop(); }

// ---------------------------------------------------------------------------
void DeviceManager::start()
{
    if (m_running) return;
    if (!m_filterInstalled) {
        QCoreApplication::instance()->installNativeEventFilter(this);
        m_filterInstalled = true;
    }
    m_running = true;
    doRefresh();
    qInfo() << "[Device] 设备监听已启动";
}

void DeviceManager::stop()
{
    if (m_filterInstalled) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
        m_filterInstalled = false;
    }
    m_running = false;
}

void DeviceManager::scheduleRefresh()
{
    if (m_running) m_debounce.start();
}

// ---------------------------------------------------------------------------
#ifdef Q_OS_WIN
bool DeviceManager::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(result)
    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG")
        return false;

    MSG* msg = static_cast<MSG*>(message);
    if (msg->message != WM_DEVICECHANGE) return false;

    switch (msg->wParam) {
    case DBT_DEVICEARRIVAL: {
        auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(msg->lParam);
        // 只关心"卷"与"设备接口"两类；DBT_DEVTYP_HANDLE 之类与我们无关
        if (hdr && (hdr->dbch_devicetype == DBT_DEVTYP_VOLUME
                    || hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)) {
            scheduleRefresh();
        }
        break;
    }
    case DBT_DEVICEREMOVECOMPLETE:
        scheduleRefresh();
        break;
    case DBT_DEVNODES_CHANGED:
        scheduleRefresh();          // 兜底：MTP 设备常只发这一类
        break;
    default:
        break;
    }
    return false;    // 返回 false 让 Qt/系统继续处理该消息
}
#else
bool DeviceManager::nativeEventFilter(const QByteArray&, void*, qintptr*)
{
    return false;
}
#endif

// ---------------------------------------------------------------------------
void DeviceManager::doRefresh()
{
    QVector<DeviceInfo> next = enumerateUsb();
    next += enumerateMtp();

    // 比较前后差异，发出插入/拔出事件（供 UI 做 toast 提示）
    QStringList nowIds;
    for (const DeviceInfo& d : next) nowIds << d.id;

    for (const DeviceInfo& d : next) {
        if (!m_knownIds.contains(d.id)) {
            qInfo() << "[Device] 已连接:" << d.name;
            emit devicePlugged(d.name);
            emit toast(QStringLiteral("检测到设备：%1").arg(d.name), QString());
        }
    }
    for (const QString& old : m_knownIds) {
        if (nowIds.contains(old)) continue;
        emit deviceRemoved(old);
    }

    m_knownIds = nowIds;
    m_devices  = next;

    QJsonArray arr;
    for (const DeviceInfo& d : m_devices) arr.append(d.toJson());
    emit devicesChanged(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

void DeviceManager::refresh() { doRefresh(); }

QString DeviceManager::devicesJson() const
{
    QJsonArray arr;
    for (const DeviceInfo& d : m_devices) arr.append(d.toJson());
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

DeviceInfo DeviceManager::deviceById(const QString& id) const
{
    for (const DeviceInfo& d : m_devices) if (d.id == id) return d;
    return DeviceInfo();
}

// ---------------------------------------------------------------------------
// USB 存储枚举
// ---------------------------------------------------------------------------
QStringList DeviceManager::removableRoots()
{
    QStringList roots;
#ifdef Q_OS_WIN
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const QString root = QString(QChar(QLatin1Char('A').unicode() + i)) + QStringLiteral(":\\");
        // DRIVE_REMOVABLE = U 盘/SD 卡；DRIVE_FIXED 里也有移动硬盘，但误判风险大，先不收
        if (GetDriveTypeW(reinterpret_cast<const wchar_t*>(root.utf16())) == DRIVE_REMOVABLE)
            roots << root;
    }
#else
    // 非 Windows 平台（仅用于开发调试）：扫 /media /Volumes
    for (const QString& base : { QStringLiteral("/media"), QStringLiteral("/Volumes") }) {
        QDir d(base);
        for (const QString& sub : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            roots << d.filePath(sub);
    }
#endif
    return roots;
}

QVector<DeviceInfo> DeviceManager::enumerateUsb() const
{
    QVector<DeviceInfo> out;

    for (const QString& root : removableRoots()) {
#ifdef Q_OS_WIN
        const std::wstring wroot = root.toStdWString();

        ULARGE_INTEGER freeBytes{}, totalBytes{}, totalFree{};
        if (!GetDiskFreeSpaceExW(wroot.c_str(), &freeBytes, &totalBytes, &totalFree))
            continue;                                  // 未就绪的读卡器会失败，跳过

        wchar_t label[256] = { 0 };
        wchar_t fsName[64] = { 0 };
        DWORD serial = 0;
        GetVolumeInformationW(wroot.c_str(), label, 256, &serial, nullptr, nullptr, fsName, 64);

        DeviceInfo d;
        // 卷序列号做 id：同一支 U 盘每次插入都稳定，比盘符可靠（盘符会变）
        d.id        = QStringLiteral("usb:%1").arg(serial, 8, 16, QLatin1Char('0'));
        d.mtp       = false;
        d.mountPath = root;
        QString name = QString::fromWCharArray(label).trimmed();
        if (name.isEmpty()) name = QStringLiteral("可移动磁盘 (%1)").arg(root.left(2));
        d.name   = name;
        d.type   = QStringLiteral("USB 存储 · %1").arg(QString::fromWCharArray(fsName));
        d.icon   = QStringLiteral("💾");
        d.totalBytes = static_cast<qint64>(totalBytes.QuadPart);
        d.freeBytes  = static_cast<qint64>(totalFree.QuadPart);
        d.supportedFormats = guessSupportedFormats(name, false);
        out.append(d);
#else
        DeviceInfo d;
        d.id = QStringLiteral("usb:") + root;
        d.name = QDir(root).dirName();
        d.type = QStringLiteral("USB 存储");
        d.icon = QStringLiteral("💾");
        d.mountPath = root;
        d.supportedFormats = guessSupportedFormats(d.name, false);
        out.append(d);
#endif
    }
    return out;
}

QVector<DeviceInfo> DeviceManager::enumerateMtp() const
{
    // MTP 设备（Android 手机 / MP3 播放器 / 智能手表）由 MtpSync 负责枚举。
    // 未编译 libmtp 时返回空列表，功能降级但不报错。
    return MtpSync::enumerateDevices();
}

// ---------------------------------------------------------------------------
// 格式支持猜测：这是启发式规则，用户可在"设置 → 设备"里手动改写。
//   - Apple 生态（iPod/iPhone）不吃 FLAC，只吃 AAC/MP3
//   - 智能手表空间小，一般只支持 MP3
//   - 老 MP3 播放器通常支持 MP3/WMA，新一点的加 FLAC
// ---------------------------------------------------------------------------
QStringList DeviceManager::guessSupportedFormats(const QString& name, bool mtp)
{
    const QString n = name.toLower();

    if (n.contains(QStringLiteral("ipod")) || n.contains(QStringLiteral("iphone"))
        || n.contains(QStringLiteral("apple")) || n.contains(QStringLiteral("ipad")))
        return { QStringLiteral("m4a"), QStringLiteral("mp3"), QStringLiteral("aac") };

    if (n.contains(QStringLiteral("watch")) || n.contains(QStringLiteral("手表"))
        || n.contains(QStringLiteral("band")) || n.contains(QStringLiteral("手环"))
        || n.contains(QStringLiteral("gt ")))
        return { QStringLiteral("mp3") };

    if (n.contains(QStringLiteral("walkman")) || n.contains(QStringLiteral("索尼"))
        || n.contains(QStringLiteral("sony")))
        return { QStringLiteral("flac"), QStringLiteral("mp3"), QStringLiteral("m4a") };

    if (n.contains(QStringLiteral("clip")) || n.contains(QStringLiteral("sport"))
        || n.contains(QStringLiteral("sandisk")))
        return { QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("m4a") };

    // MTP 设备（安卓）基本都能吃 FLAC；USB 存储的未知设备保守给 mp3+flac
    if (mtp) return { QStringLiteral("flac"), QStringLiteral("mp3"), QStringLiteral("m4a") };
    return { QStringLiteral("mp3"), QStringLiteral("flac") };
}

} // namespace ms
