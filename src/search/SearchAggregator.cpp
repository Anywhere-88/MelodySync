// ============================================================================
//  src/search/SearchAggregator.cpp
// ============================================================================
#include "search/SearchAggregator.h"

#include "core/Async.h"
#include "search/sources/PreviewSource.h"
#include "search/sources/WebSource.h"

#include <QDebug>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace ms {

QJsonObject SearchAggregator::Result::toJson() const
{
    QJsonArray arr;
    for (const SearchHit& h : hits) arr.append(h.toJson());

    QJsonObject o;
    o[QStringLiteral("ok")]       = true;
    o[QStringLiteral("intent")]   = intent.toJson();
    o[QStringLiteral("engine")]   = engine;
    o[QStringLiteral("summary")]  = summary;
    o[QStringLiteral("total")]    = hits.size();
    o[QStringLiteral("rawCount")] = rawCount;
    o[QStringLiteral("free")]     = freeCount;
    o[QStringLiteral("lossless")] = losslessCount;
    o[QStringLiteral("results")]  = arr;
    return o;
}

// ---------------------------------------------------------------------------
SearchAggregator::SearchAggregator(QObject* parent) : QObject(parent)
{
    registerDefaults();
}

void SearchAggregator::registerDefaults()
{
    // 结构：两层三个源，但**默认只启用全网**（见 Settings 的 kDefaultSources）。
    //   第一层（平台直连，可选）：网易云 / QQ 音乐 —— 走平台公开 HTTP 接口，
    //   秒级返回，元数据准确，免费曲目直接换到官方音频直链。默认**不勾选**：
    //   它们最像原唱的那条常常下不了（会员/无版权），而次优的同名翻唱、合集
    //   却能下载、分数也不低 —— 用户看到的就是"原唱下不了、其他都是翻唱"。
    //   对一个下载器来说，"搜到的都能下"比"元数据更准"重要，所以搜索与换源
    //   统一走全网。想要"先平台再全网"的老行为，在设置页把这两个勾上即可。
    //   第二层（全网）：yt-dlp 级联哔哩哔哩 / YouTube / SoundCloud，覆盖面最广，
    //   返回的每条都是可下载的页面地址。
    //
    // 为什么砍掉了 Jamendo / FMA / Internet Archive（以及早已下线的 Pixabay）：
    //   它们对国内用户要么连不上（FMA / archive 稳定 8 秒超时）、要么要去官网
    //   申请 client_id 才有结果。勾了等于没勾，却要在界面上占三个徽标、
    //   在设置里占三个勾选框，还要用户自己猜"为什么勾了没结果"。
    //   免费 CC 音乐的需求由「全网」源覆盖（YouTube/SoundCloud 上有大量 CC 内容）。
    //   这四个源的实现文件保留在 sources/ 下不删：将来若要接一个真能用的
    //   CC 源（例如带 client_id 的 Jamendo），照着改一行注册即可。
    for (const SourceMeta& m : PreviewSource::allMetas()) {
        auto src = std::make_shared<PreviewSource>(m.id);
        m_sources.append(src);
        collectMeta(m);
    }

    {
        auto web = std::make_shared<WebSource>();
        m_sources.append(web);
        collectMeta(web->meta());
    }

    qInfo() << "[Aggregator] 已注册" << m_sources.size() << "个搜索源:";
    for (const auto& s : m_sources)
        qInfo() << "   -" << s->meta().id << s->meta().name
                << "(tier" << s->meta().tier << ")";
}

QHash<QString, SourceMeta> SearchAggregator::allMetas() const { return m_metas; }

QVector<SearchAggregator::SourceStatus> SearchAggregator::sourceStatus() const
{
    QVector<SourceStatus> out;
    out.reserve(m_sources.size());
    for (const auto& s : m_sources) {
        SourceStatus st;
        st.id        = s->meta().id;
        st.available = s->available();
        if (!st.available) st.reason = s->unavailableReason();
        out.append(st);
    }
    return out;
}

// ---------------------------------------------------------------------------
SearchAggregator::Result SearchAggregator::searchAll(const SearchIntent& intent,
                                                     const CancelTokenPtr& cancel)
{
    Result result;
    result.intent = intent;

    // 1) 挑出本次要跑的源（配置里启用的 ∩ 实际注册的）
    QVector<std::shared_ptr<SearchSource>> active;
    for (const auto& s : m_sources) {
        if (m_cfg.enabledSources.isEmpty() || m_cfg.enabledSources.contains(s->meta().id))
            active.append(s);
    }
    if (active.isEmpty()) {
        result.summary = QStringLiteral("没有启用任何搜索源，请到「设置 → 搜索源」勾选至少一个。");
        return result;
    }

    // 2) 分层执行 + 总超时。
    //
    //    默认配置下**只有全网一层**（tier 1），一次搜索就是起 yt-dlp 去视频站
    //    抓一批候选，约 9~13 秒。对下载器来说这是对的取舍：返回的每一条都能下。
    //
    //    如果用户在设置页勾回了平台源，就恢复"先平台、再全网"的两层流程：
    //    ① 「先找网易云 + QQ」：第一层（tier 0）是两个平台直连源，走 HTTP，
    //        秒级返回，元数据准。**先跑完它们**，再决定要不要惊动第二层。
    //    ② 「再找全网」：只有当第一层"不够用"或"最像原唱的那条下不了"时才跑
    //        （判据见 needWeb 那一段）——搜一首平台本来就免费的歌，3 秒就结束。
    //
    //    总超时仍然必须存在：任何一层的任何源都可能挂住，到点就取消在跑的源
    //    （yt-dlp 会被杀掉进程，阻塞中的 HTTP 请求受各自超时约束尽快返回），
    //    整体在 searchTimeoutMs 内必定返回，不再"假死"。
    //
    //    cancel 允许为空（探针等直调方不建令牌），但总超时必须永远可执行：
    //    这里兜底建一个内部令牌。曾经漏了这一步，超时分支 `if (cancel)` 静默
    //    跳过，实测整次搜索拖到 63 秒。
    const CancelTokenPtr effectiveCancel =
        cancel ? cancel : std::make_shared<CancelToken>();

    // 按层次排好：tier 0 在前。层的边界记下来，供两阶段执行使用。
    std::stable_sort(active.begin(), active.end(),
                     [](const std::shared_ptr<SearchSource>& a,
                        const std::shared_ptr<SearchSource>& b) {
        return a->meta().tier < b->meta().tier;
    });
    int tierSplit = 0;
    while (tierSplit < active.size() && active[tierSplit]->meta().tier == 0) ++tierSplit;

    QVector<QVector<SearchHit>> buckets(active.size());
    // 每个源是否已向前端发过终态（done/failed）。总超时后聚合器要给没跑完的
    // 源补发 failed；迟到的真实完成必须被 exchange 拦下，不能覆盖终态。
    std::vector<std::atomic<bool>> reported(active.size());

    // 所有信号统一是“跨线程 queued”，由 Qt 保证在 UI 线程执行
    auto emitProgress = [this](const QString& id, const QString& st, int n) {
        emit sourceProgress(id, st, n);
    };

    const qint64 deadlineMs = qBound(4000, m_cfg.searchTimeoutMs, 60000);
    QDeadlineTimer deadline(deadlineMs);
    std::atomic<int> doneCount{ 0 };

    auto worker = [&](int idx) {
        const auto& src = active[idx];
        const QString sid = src->meta().id;
        QElapsedTimer clock;
        clock.start();

        SearchContext ctx;
        ctx.intent = intent;
        ctx.limitPerSource = m_cfg.limitPerSource;
        ctx.cancel = effectiveCancel;
        // 把总超时当作本源的预算传下去：全网源内部要串行跑多个站点，
        // 没有预算的话第一个不可达的站点就能吃光整次搜索的时间。
        ctx.budgetMs = deadlineMs;

        QVector<SearchHit> hits;
        try {
            if (src->available()) {
                hits = src->search(ctx);
            } else {
                qInfo() << "[Aggregator]" << sid << "不可用:" << src->unavailableReason();
            }
        } catch (const std::exception& e) {
            // 单个源的异常绝不允许冒泡——否则整个搜索就废了
            qWarning() << "[Aggregator]" << sid << "抛出异常:" << e.what();
        } catch (...) {
            qWarning() << "[Aggregator]" << sid << "抛出未知异常";
        }

        // 原语言兜底重试：LLM 识别出"用户用译名搜非中文歌"时，平台曲库里
        // 存的是原名，中文查询要么 0 命中、要么命中一堆翻唱 —— 原唱只能
        // 靠原语言检索词（如"YOASOBI アイドル"）才找得到。
        // 所以不能只在"命中 <3 条"时才重试：命中再多，可能全是翻唱。
        //   tier 0（网易云/QQ，单次 1~3 秒）：只要 LLM 给了 alt 就补一轮
        //   tier 1（全网源走串行 yt-dlp 闸门，一次 9~13 秒）：
        //   维持"命中 <3 才试"，且要求剩余时间足够 —— 它的预算本来就紧
        const bool fastSrc = src->meta().tier == 0;
        const bool wantRetry = !intent.altQueries.isEmpty()
            && src->available() && !effectiveCancel->isCanceled()
            && (fastSrc ? clock.elapsed() < deadlineMs / 3
                        : hits.size() < 3 && clock.elapsed() < deadlineMs / 2);
        if (wantRetry && deadline.remainingTime() > 5000) {
            const int maxTries = fastSrc ? 1 : 2;        // 快源只补 1 轮，控制耗时
            for (int a = 0; a < intent.altQueries.size() && a < maxTries; ++a) {
                if (!fastSrc && hits.size() >= 3) break;
                // 原语言变体和主检索词一模一样时，这一轮纯属白跑：
                // LLM 给出的 altQueries 经常就是"歌手 歌名"，与 platformSearchText()
                // 一字不差（实测《だから僕は音楽を辞めた》就是这个情形，两个检索词
                // 完全相同）。重试等于把同一个请求打两遍 —— 既拖慢搜索，又特别容易
                // 撞上平台限流：QQ 搜索接口连打两次就回 req.code=2001 且列表为空
                // （实测间隔 45 秒重试即恢复），日志上看起来像"QQ 没有这首歌"。
                if (intent.altQueries.at(a).trimmed().compare(
                        intent.platformSearchText().trimmed(), Qt::CaseInsensitive) == 0)
                    continue;
                SearchContext altCtx = ctx;
                // altQueries 是完整词组（含原语言歌手），清掉 artist/name
                // 让 platformSearchText() 原样输出它
                altCtx.intent = intent;
                altCtx.intent.name = intent.altQueries.at(a);
                altCtx.intent.artist.clear();
                QVector<SearchHit> more;
                try {
                    more = src->search(altCtx);
                } catch (...) {
                    break;
                }
                if (!more.isEmpty()) {
                    qInfo() << "[Aggregator]" << sid << "原语言重试命中"
                            << more.size() << "条（" << intent.altQueries.at(a) << "）";
                    hits += more;
                }
            }
        }

        // 记录源内排名（热度微调分用，见 IntentParser::scoreAll）
        for (int i = 0; i < hits.size(); ++i) hits[i].sourceRank = i;

        buckets[idx] = hits;
        const int n = hits.size();

        // 终态只发一次：如果总超时已经补发过 failed，这里就不再发迟到的
        // done/failed，前端的状态徽标不会"复活"。
        if (!reported[idx].exchange(true)) {
            emitProgress(sid, n > 0 ? QStringLiteral("done") : QStringLiteral("failed"), n);

            // 增量下发该源结果：前端可以边搜边渲染
            QJsonArray arr;
            for (const SearchHit& h : hits) arr.append(h.toJson());
            emit sourceResults(sid, QString::fromUtf8(
                QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        }

        doneCount.fetch_add(1, std::memory_order_release);
    };

    // 跑一批源（[begin, end)），等它们全部结束或总超时到点。
    // 每个源一个线程（源数量本就很少，且各自网络 IO 受限，不值得更细的池化）
    auto runBatch = [&](int begin, int end) {
        if (begin >= end) return;
        for (int i = begin; i < end; ++i)
            emitProgress(active[i]->meta().id, QStringLiteral("run"), 0);

        std::vector<std::thread> pool;
        pool.reserve(end - begin);
        for (int i = begin; i < end; ++i)
            pool.emplace_back(worker, i);

        while (doneCount.load(std::memory_order_acquire) < end && !deadline.hasExpired())
            std::this_thread::sleep_for(std::chrono::milliseconds(40));

        if (deadline.hasExpired() && doneCount.load(std::memory_order_acquire) < end) {
            qInfo() << "[Aggregator] 搜索超过" << deadlineMs << "ms，取消仍在运行的"
                    << (end - doneCount.load()) << "个源";
            effectiveCancel->cancel();

            // 立刻给没跑完的源补发终态，前端徽标才不会永远停在"搜索中"。
            // （exchange 保证与迟到的真实完成只发一次，见 worker 里的同款逻辑。）
            for (int i = begin; i < end; ++i) {
                if (!reported[i].exchange(true))
                    emitProgress(active[i]->meta().id, QStringLiteral("failed"), 0);
            }
        }

        // 收尾：等本批线程退出。慢源收到 cancel 后尽快结束（yt-dlp 杀进程；
        // 阻塞中的 HTTP 请求受各自超时约束，最多再等一个超时周期）。
        for (std::thread& t : pool)
            if (t.joinable()) t.join();
    };

    // ---- 第一层：平台直连（只有设置里勾了 netease/qq 才存在）----
    runBatch(0, tierSplit);

    // ---- 判断要不要跑全网层 ----
    //
    // 判据：第一层已经给出"足够好的、能直接下载的**完整**结果"吗？
    //   有 ≥3 条可下载结果，且其中最佳匹配度 ≥ kSkipWebScore 且那条是完整时长
    //   → 平台已经够了，不必再花 10 秒去全网找（用户要的"先平台、再全网"里，
    //     "再"是兜底的意思）。
    //   否则（平台全是仅试听 / 只有 1:29 试听版 / 压根没这首）→ 跑全网。
    //   "完整时长"这一条是实测补的：搜 YOASOBI《アイドル》时 QQ 给出 1:29 试听版
    //   加满分，第一层判"已足够"跳过了全网，用户拿到的头条就是一段试听 —— 而
    //   "平台拿不到完整版"正是最该去全网碰运气的情形。
    // 打分用 scoreAll 现算一遍：此处只是为了判断，后面还会用完整的
    // 去重 + 评分流程重算最终分数，不会有副作用。
    bool needWeb = false;
    if (tierSplit == 0) {
        // 没启用任何平台源（默认的「仅全网」模式）：全网就是唯一的一层，直接跑。
        // 这里**不能**走下面那套"第一层够不够"的判据 —— 它的前提是"手上有一批
        // 平台结果可以比较"，喂一个空集进去只会恒定为真，还会打出
        // "第一层不足（可下载 0 条…）→ 继续找全网"这种既吓人又没信息量的日志，
        // 让人以为搜索出了问题。
        needWeb = true;
    } else if (tierSplit < active.size()) {
        QVector<SearchHit> t0;
        for (int i = 0; i < tierSplit; ++i) t0 += buckets[i];
        IntentParser::scoreAll(t0, intent, 0, m_metas);

        constexpr double kSkipWebScore  = 72.0;
        // 低于这个秒数基本就是试听片段，不算"拿到了这首歌"
        constexpr int    kFullTrackSec  = 90;
        // 「最佳不可下载」要比「最佳可下载」至少高出这么多分，才值得再花
        // 10 秒去全网跑一趟 —— 只差一两分说明平台上已经有同等好的可下载版本。
        constexpr double kUnavailableGap = 3.0;
        int downloadable = 0;
        double bestDl = 0.0;
        double bestDlFull = 0.0;        // 只统计完整时长的那部分
        double bestAny = 0.0;           // 全场最佳（含"仅试听"）
        bool bestAnyDownloadable = true;
        for (const SearchHit& h : t0) {
            const bool dl = !h.downloadUrl.isEmpty() && !h.previewOnly;
            if (h.score > bestAny) {
                bestAny = h.score;
                bestAnyDownloadable = dl;
            }
            if (!dl) continue;
            ++downloadable;
            bestDl = std::max(bestDl, h.score);
            if (h.durationSec >= kFullTrackSec)
                bestDlFull = std::max(bestDlFull, h.score);
        }
        needWeb = !(downloadable >= 3 && bestDl >= kSkipWebScore
                    && bestDlFull >= kSkipWebScore)
                  // ---- 判据之二：匹配度最高的那条如果下不了，就必须去全网 ----
                  // 前一条只看"能下载的里面有没有够好的"，会漏掉最要命的那种情形：
                  // 平台上**最像这首歌的那条**恰好是不可下载的（会员曲/仅试听），
                  // 而次优的几条（同名翻唱、混音）能下载、分数也不低 —— 于是判定
                  // "第一层已足够"、把全网整个跳过，用户只看到一堆翻唱，
                  // 永远不知道视频站上有能下载的官方版。
                  //
                  // 实测《だから僕は音楽を辞めた》（ヨルシカ），差别只在有没有歌手字段：
                  //   只搜歌名 → intent.artist 为空 → 评分对翻唱不设防（原唱一致度
                  //              取中性 0.75、歌手闸门不触发）→ 免费翻唱拿到 78 分
                  //              → "已足够（可下载 20 条，最佳 78 分）"→ 跳过全网
                  //              → 用户看到的只有网易云的"仅试听"原唱 + 翻唱；
                  //   带歌手搜 → 翻唱撞歌手闸门 ×0.60 → "不足（最佳 37 分）"
                  //              → 跑全网 → 拿到 B 站官方 MV **88 分、可下载**。
                  // 用户对这件事的描述是"搜出来原唱下不了，其他结果也没有原唱"。
                  || (!bestAnyDownloadable && bestAny >= kSkipWebScore
                      && bestAny - bestDl >= kUnavailableGap);

        if (needWeb)
            qInfo() << "[Aggregator] 第一层不足（可下载" << downloadable
                    << "条，最佳可下载" << static_cast<int>(bestDl)
                    << "分/完整时长" << static_cast<int>(bestDlFull)
                    << "分，全场最佳" << static_cast<int>(bestAny)
                    << (bestAnyDownloadable ? "分可下载" : "分不可下载")
                    << "）→ 继续找全网";
        else
            qInfo() << "[Aggregator] 第一层已足够（可下载" << downloadable
                    << "条，最佳" << static_cast<int>(bestDl) << "分）→ 跳过全网源";

        for (int i = tierSplit; i < active.size(); ++i) {
            if (needWeb) break;
            // 跳过的源也必须发终态，否则前端徽标永远停在"搜索中"。
            // 状态用 "skip"（前端显示"已跳过"），而不是 done/0 —— 后者会被
            // 读成"搜了，一条都没有"，与事实不符。
            if (!reported[i].exchange(true))
                emitProgress(active[i]->meta().id, QStringLiteral("skip"), 0);
        }
    }

    // ---- 全网层 ----
    if (needWeb) {
        if (tierSplit == 0)
            qInfo() << "[Aggregator] 仅全网模式（未启用平台源）→ 直接跑全网";
        runBatch(tierSplit, active.size());
    }

    // 3) 合并所有桶
    QVector<SearchHit> merged;
    for (const QVector<SearchHit>& b : buckets) merged += b;
    result.rawCount = merged.size();

    // 4) 智能去重（同曲多源合并，优先无损）
    IntentParser::dedupe(merged);

    // 5) AI 匹配度评分（时长参考值由函数内部用中位数推导）
    IntentParser::scoreAll(merged, intent, 0, m_metas);

    // 6) 排序：可下载优先于仅试听；其余按 匹配度 → 无损 → 免费。
    //    为什么把 previewOnly 放在第一优先级：用户搜"周杰伦 晴天"时，
    //    QQ 原曲匹配 93 分但仅试听，翻唱 69 分但可下载 —— 按 匹配度 排，
    //    前三名全是"仅试听"，用户的观感就是"AI 搜到的歌都只能试听"。
    //    对一个下载器来说，"下得到"比"分更高"重要：仅试听的条目本来就
    //    只有试听一个用途，往下排并不损失什么。
    std::sort(merged.begin(), merged.end(), [](const SearchHit& a, const SearchHit& b) {
        if (a.previewOnly != b.previewOnly) return a.previewOnly < b.previewOnly;
        if (std::abs(a.score - b.score) > 1.0) return a.score > b.score;
        const int qa = qualityRank(a.quality), qb = qualityRank(b.quality);
        if (qa != qb) return qa > qb;
        if (a.free != b.free) return a.free;
        return a.sourceId < b.sourceId;
    });

    // 7) 统计 + 摘要
    for (const SearchHit& h : merged) {
        if (h.free) ++result.freeCount;
        if (qualityRank(h.quality) >= qualityRank(Quality::Lossless)) ++result.losslessCount;
    }
    result.hits    = merged;
    result.summary = IntentParser::buildSummary(merged, intent);

    qInfo() << "[Aggregator] 聚合完成：原始" << result.rawCount
            << "条 → 去重后" << merged.size() << "条";
    return result;
}

} // namespace ms
