// ============================================================================
//  src/search/SearchAggregator.h
//  多源聚合搜索（两层三个源）。
//
//  设计要点：
//   1) 两层执行：第一层（网易云 / QQ）并行跑完并评估；只有它给不出"能直接
//      下载的"结果时才跑第二层（全网源，yt-dlp 级联各站点）。见 searchAll()。
//   2) 每完成一个源就立刻用信号把"该源命中 N 条"推给前端，
//      前端因此能显示逐源进度（而不是干等一个转圈）；
//   3) 单源异常不影响整体——catch 掉、返回空、继续聚合，这是聚合器的生命线；
//   4) 总超时兜底：到点取消仍在跑的源，整体必定在 searchTimeoutMs 内返回。
// ============================================================================
#pragma once

#include "ai/IntentParser.h"
#include "core/Types.h"
#include "search/SearchSource.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

namespace ms {

class SearchAggregator : public QObject
{
    Q_OBJECT

public:
    struct Config
    {
        QStringList enabledSources;
        int     limitPerSource = 12;
        int     maxParallel = 8;
        // 搜索总超时（毫秒）：两层执行的硬上限，到点取消仍在跑的源。
        //   第一层（网易云/QQ 走 HTTP）通常 2~3 秒；第二层（全网源）是
        //   yt-dlp 驱动的，起进程 + 逐条抓页面一个回合 9~13 秒，所以这个值
        //   实际上就是"要不要跑全网"的时间预算。
        //   到点后 yt-dlp 会被杀掉进程，已输出的条目仍作为部分结果保留。
        int     searchTimeoutMs = 20000;
    };

    struct Result
    {
        SearchIntent intent;
        QVector<SearchHit> hits;        // 已去重 + 已评分 + 已排序
        QString summary;                // AI 推荐摘要（含 HTML 标签）
        QString engine;                 // 意图解析引擎：llm:OpenAI / rule
        int     rawCount = 0;           // 去重前的原始条数
        int     freeCount = 0;
        int     losslessCount = 0;
        QJsonObject toJson() const;
    };

    // 单个源当前的可用性。前端"搜索源"设置页要能直接告诉用户
    // "Jamendo 需要 client_id""yt-dlp 已损坏"，而不是让用户对着一堆
    // 勾选框猜为什么搜不到东西。
    struct SourceStatus
    {
        QString id;
        bool    available = true;
        QString reason;
    };

    explicit SearchAggregator(QObject* parent = nullptr);

    void setConfig(const Config& c) { m_cfg = c; }
    Config config() const { return m_cfg; }

    // 同步执行（务必在后台线程里调用）。返回值可直接转 JSON 下发前端。
    Result searchAll(const SearchIntent& intent, const CancelTokenPtr& cancel = CancelTokenPtr());

    // 已注册源的全部元信息（前端"搜索源"设置页 + 评分用）
    QHash<QString, SourceMeta> allMetas() const;

    // 每个已注册源的可用性 + 不可用原因
    QVector<SourceStatus> sourceStatus() const;

signals:
    // status: run / done / failed —— 与前端 updateSrcStatus() 的 CSS class 对齐
    void sourceProgress(const QString& sourceId, const QString& status, int count);
    // 每个源搜完后立即推送该源的原始结果（前端可增量渲染）
    void sourceResults(const QString& sourceId, const QString& hitsJson);

private:
    void registerDefaults();
    void collectMeta(const SourceMeta& m) { m_metas.insert(m.id, m); }

    Config m_cfg;
    QHash<QString, SourceMeta> m_metas;
    QVector<std::shared_ptr<SearchSource>> m_sources;
};

} // namespace ms
