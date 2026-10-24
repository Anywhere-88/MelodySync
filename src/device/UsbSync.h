// ============================================================================
//  src/device/UsbSync.h
//  USB 存储（U 盘 / SD 卡 / 移动硬盘）同步 + 增量判定 + 格式适配。
//
//  ★ 增量同步算法（见 cpp 内详细注释）：
//      跳过条件 = 目标文件存在 ∧ 大小相同 ∧ 修改时间差 ≤ 容差
//      容差默认 2 秒 —— 因为 FAT32/exFAT 的时间戳精度只有 2 秒，
//      用严格的毫秒比较会导致"每次同步都全量重拷"的经典 bug。
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

class UsbSync : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        QString targetSubdir = QStringLiteral("Music");
        bool    incremental = true;      // 是否启用增量跳过
        bool    convert = false;         // 设备不支持源格式时是否转码
        int     targetBitrate = 320;     // 转码码率
        int     mtimeToleranceSec = 2;   // 时间戳容差（FAT32 精度）
        bool    verifyAfterCopy = true;  // 拷完后校验大小
    };

    struct PlanItem
    {
        QString sourcePath;
        QString targetPath;
        QString fileName;
        qint64  sizeBytes = 0;
        bool    skip = false;
        QString skipReason;      // "已存在且一致" / "大小不同，需重拷"
        bool    needConvert = false;
        QString convertFormat;   // mp3 / m4a
    };

    struct Plan
    {
        QString sourceDir;
        QString targetRoot;
        QVector<PlanItem> items;
        int totalCount = 0;
        int skipCount = 0;
        int copyCount = 0;
        int convertCount = 0;
        qint64 copyBytes = 0;
        QString toJson() const;
    };

    explicit UsbSync(QObject* parent = nullptr);

    // 生成同步计划（只读磁盘，不写任何东西，方便 UI 先给用户确认）
    static Plan buildPlan(const QString& sourceDir,
                          const DeviceInfo& device,
                          const Options& opt,
                          const CancelTokenPtr& cancel = CancelTokenPtr());

    // 选择性同步：用调用方给定的文件清单生成计划（不再扫描 sourceDir）。
    // 「勾选歌曲再同步」功能走这条路 —— 文件清单由 WebBridge 按音乐库 id 解析出来。
    // files 里的非音频/不存在的路径会被静默跳过（isAudioFile + exists 双重过滤）。
    static Plan buildPlanForFiles(const QStringList& files,
                                  const QString& sourceDir,
                                  const DeviceInfo& device,
                                  const Options& opt,
                                  const CancelTokenPtr& cancel = CancelTokenPtr());

    // 执行计划。onProgress(percent, currentIndex, totalCount, currentFileName)
    bool execute(const Plan& plan,
                 const Options& opt,
                 const std::function<void(int, int, int, const QString&)>& onProgress,
                 const CancelTokenPtr& cancel,
                 QString* errorOut);

    // 递归收集目录下所有音频文件
    static QStringList collectAudioFiles(const QString& dir, const CancelTokenPtr& cancel = CancelTokenPtr());

    // 判断扩展名是否属于我们管理的音频类型
    static bool isAudioFile(const QString& fileName);

private:
    static bool compareItem(const PlanItem& it, const Options& opt, QString* reason);
};

} // namespace ms
