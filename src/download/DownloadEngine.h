// ============================================================================
//  src/download/DownloadEngine.h
//  下载引擎：并发调度 / 暂停恢复 / 失败重试 / 元数据写入 / 自动入库。
//
//  调度模型（重要）：
//    - 一个"任务池 + 固定数量 worker 槽位"的简单模型，而不是每任务一线程。
//      concurrency 默认 3、可配 1-8，超出槽位的任务留在 Pending 队列里等；
//    - 任何时刻只有 pump() 会改状态，pump() 本身受互斥锁保护，
//      因此不会出现"同一个任务被两个线程同时下载"的经典事故；
//    - 暂停 = 把状态改回 Pending 并置 paused 标记，worker 在进度回调里看到
//      取消令牌后退出（HTTP 直链与 yt-dlp 两条路径都支持中断）。
// ============================================================================
#pragma once

#include "core/Async.h"
#include "core/Types.h"

#include <QMutex>
#include <QObject>
#include <QVector>

#include <memory>

namespace ms {

class DownloadEngine : public QObject
{
    Q_OBJECT

public:
    explicit DownloadEngine(QObject* parent = nullptr);
    ~DownloadEngine() override;

    void setUserId(qint64 userId) { m_userId = userId; }
    qint64 userId() const { return m_userId; }

    void setConcurrency(int n);
    int  concurrency() const;

    // 入队（自动去重：同 歌名+歌手+音质 已在队列中则跳过）
    int  enqueue(const QVector<DownloadTask>& tasks);
    void startAll();                        // 开始 / 继续全部
    void pauseAll();                        // 暂停全部进行中的
    void cancelTask(const QString& id);     // 取消单条
    void removeTask(const QString& id);     // 从队列移除（不删文件）
    void clearDone();                       // 清除已完成
    void clearAll();                        // 清空队列
    void retryFailed();

    QVector<DownloadTask> tasks() const;
    QString queueJson() const;

    // 一次下载的完成结果（同步执行，供"设备同步前预下载"复用）
    struct Outcome
    {
        bool ok = false;
        QString filePath;
        QString error;
        // 不可重试的失败：平台明确说"要会员/本地区无版权"。
        // 这类错误重试 100 次结果一样，只会白白刷满日志、拖慢整个队列，
        // 所以 runTask 看到它就立刻收手，把原因原样交给用户。
        bool    fatal = false;
        // 成功时的补充说明（非空表示这次成功走了"换源"路径，
        // 例如：平台拒绝直链，已从哔哩哔哩换源下载）。
        QString note;
    };
    Outcome downloadOne(const DownloadTask& task,
                        const std::function<void(int percent, qint64 speed)>& onProgress,
                        const CancelTokenPtr& cancel);

signals:
    void queueUpdated(const QString& json);
    void progress(const QString& id, int percent, qint64 speed);
    void libraryUpdated(const QString& json);
    void toast(const QString& message, const QString& type);
    void taskFinished(const QString& id, const QString& filePath);

private:
    void pump();
    void runTask(int index);
    void emitQueue();
    void emitLibrary();
    QString buildSavePath(const DownloadTask& task, const QString& ext) const;
    bool isDuplicate(const DownloadTask& t) const;

    mutable QMutex m_mutex;
    QVector<DownloadTask> m_tasks;
    QVector<CancelTokenPtr> m_cancels;
    QVector<bool> m_running;
    int m_concurrency = 3;
    int m_active = 0;
    qint64 m_userId = 0;
    bool m_pumping = false;
};

} // namespace ms
