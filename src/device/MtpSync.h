// ============================================================================
//  src/device/MtpSync.h
//  MTP 设备同步（Android 手机 / MP3 播放器 / 智能手表）。
//
//  MTP 与 U 盘的本质区别：MTP 设备没有盘符，不能直接用 std::filesystem 拷贝，
//  必须走 libmtp 的对象传输协议。因此本文件在编译期用 MS_HAVE_LIBMTP 做开关：
//    - 有 libmtp：完整功能（枚举 / 建目录 / 传文件 / 进度回调）
//    - 无 libmtp：只做"识别提示"，UI 上给出明确原因而不是静默失败
// ============================================================================
#pragma once

#include "core/Async.h"
#include "core/Types.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace ms {

class MtpSync : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        QString targetFolder = QStringLiteral("Music");   // 设备上的目标目录
        QStringList existingNames;                        // 增量同步：设备上已有的文件名
        int     storageId = 0;                            // 0 = 自动选择第一个可用存储
    };

    // 枚举系统里的 MTP 设备（不需要长连接，用于设备列表展示）
    static QVector<DeviceInfo> enumerateDevices();

    // 列出设备上某个目录里的文件名（用于增量同步比对）
    static QStringList listRemoteFiles(const QString& deviceId, const QString& folder,
                                       QString* errorOut = nullptr);

    // 推送文件。onProgress(percent, current, total, currentFileName)
    static bool pushFiles(const QString& deviceId,
                          const QStringList& localFiles,
                          const Options& opt,
                          const std::function<void(int, int, int, const QString&)>& onProgress,
                          const CancelTokenPtr& cancel,
                          QString* errorOut);
};

} // namespace ms
