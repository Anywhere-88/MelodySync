// ============================================================================
//  src/search/SearchSource.h
//  搜索源统一接口。SearchAggregator 只认这个接口，
//  新增一个源 = 新增一个子类 + 在 aggregator 里注册一行。
// ============================================================================
#pragma once

#include "core/Async.h"
#include "core/Types.h"

#include <QString>
#include <QVector>

namespace ms {

struct SearchContext
{
    SearchIntent intent;
    int          limitPerSource = 12;      // 每个源最多取多少条
    CancelTokenPtr cancel;
    QString      httpProxy;                // 预留：企业环境代理
    // 本源的软预算（毫秒，0 = 不限）。聚合器填的就是本次搜索的总超时。
    // 为什么源需要知道它：全网源内部要**串行**跑多个站点（yt-dlp 进程本身
    // 也是串行的，见 YtDlpWrapper 的 ProcGate），站点不可达时一个就能挂
    // 30~44 秒。没有预算的话，第一个慢站会把后面所有站点的时间吃光，
    // 整个源必然被总超时掐死、一条结果都出不来。
    qint64       budgetMs = 0;
};

class SearchSource
{
public:
    virtual ~SearchSource() = default;

    // 源的元信息（id / 名称 / 可信度 / 是否免费），参与 AI 评分
    virtual SourceMeta meta() const = 0;

    // 同步执行搜索。调用方保证在后台线程中调用。
    // 约定：任何异常/网络失败都必须自行捕获并返回空列表，绝不抛给上层。
    virtual QVector<SearchHit> search(const SearchContext& ctx) = 0;

    // 该源是否已经可用（例如需要 API Key 但未配置 → 返回 false，前端可显示"未配置"）
    virtual bool available() const { return true; }

    // 不可用的原因（前端用于提示）
    virtual QString unavailableReason() const { return QString(); }
};

} // namespace ms
