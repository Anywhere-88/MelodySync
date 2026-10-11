// ============================================================================
//  src/search/sources/WebSource.cpp
// ============================================================================
#include "search/sources/WebSource.h"

#include "ai/IntentParser.h"          // creditedArtist：标题里的歌手名 vs uploader
#include "download/YtDlpWrapper.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QTcpSocket>
#include <QThread>

#include <atomic>
#include <thread>

namespace ms {

namespace {

// 站点表。顺序 = 尝试顺序，**不要随意调整**：
//   哔哩哔哩必须第一。它是国内唯一稳定可直连的站点，而且搜索接口在结果页里
//   不带标题，只能走完整模式逐条抓页面（最慢，6 条约 9 秒）—— 得先给它最充裕的
//   预算。后面的 YouTube / SoundCloud 在无代理环境下必然连不上，只作为"有代理
//   的用户"的加分项，拿剩余时间试一下就走。
struct Site {
    const char* id;        // 内部标识（拼进全局 id，保证跨站点不撞号）
    const char* prefix;    // yt-dlp 搜索伪协议
    const char* label;     // 展示名（写进 license，界面上作为一个标签出现）
    const char* host;      // 连通性预检用的主机名（空 = 不预检）
    bool        flat;      // 能否用 --flat-playlist（哔哩哔哩不能，见 YtDlpWrapper.h）
    int         limit;     // 该站点最多取多少条
};

const Site kSites[] = {
    { "bilibili",   "bilisearch", "哔哩哔哩",   "api.bilibili.com", false, 6 },
    { "youtube",    "ytsearch",   "YouTube",    "www.youtube.com",  true,  8 },
    { "soundcloud", "scsearch",   "SoundCloud", "soundcloud.com",   true,  6 },
};

constexpr qint64 kSiteMinBudgetMs   = 5000;              // 剩余少于这个值就不开新站点
constexpr qint64 kExtraSiteBudgetMs = 7000;              // 首个站点之后每个站点的上限
constexpr qint64 kCoolDownMs        = 10 * 60 * 1000;    // 不可达站点的冷却时长
constexpr int    kProbeTimeoutMs    = 2500;              // 连通性预检的 TCP 超时

// ---------------------------------------------------------------------------
// 连通性预检
// ---------------------------------------------------------------------------
// 为什么要在起 yt-dlp 之前先做一次 TCP 连通性探测：
//   yt-dlp 在站点不可达时要耗到自己内部重试用尽才放弃（实测 30~44 秒）。
//   把"不可达"这个结论从"跑一次 yt-dlp 才知道"提前到"2.5 秒的 TCP 连接"，
//   是"搜索总是卡在 YouTube Music"那个老问题最直接的解法：
//   实测预检失败会在几百毫秒内返回（被墙的域名通常直接 RST/超时），
//   于是 YouTube / SoundCloud 一共只花掉 1~2 秒就被跳过，不再吃掉 12 秒预算。
//   预检只排除"明显连不上"的站点；连得上但握手被干扰的情况仍由 yt-dlp 兜底。
bool hostReachable(const QString& host)
{
    if (host.isEmpty()) return true;      // 没配主机名 = 不预检
    QTcpSocket sock;
    sock.connectToHost(host, 443);
    return sock.waitForConnected(kProbeTimeoutMs);
}

// ---------------------------------------------------------------------------
// 不可达冷却表（进程内）
// ---------------------------------------------------------------------------
// 为什么需要它：yt-dlp 在站点不可达时要耗到自己内部重试用尽才放弃（实测
// 30~44 秒）。过去三个源名义上并行、实际抢同一把串行闸门，YouTube 一旦抢到
// 就会把哔哩哔哩饿死到总超时——用户看到的现象就是"搜什么都要等 20 秒，
// 而且除网易云/QQ 全 failed"。
// 有了冷却表，不可达的站点只在第一次搜索时付一次代价，之后 10 分钟内直接跳过。
QHash<QString, qint64> g_coolUntil;      // prefix → 冷却截止时刻（ms epoch）
QMutex                 g_coolMutex;

bool coolingDown(const QString& prefix)
{
    QMutexLocker lock(&g_coolMutex);
    return g_coolUntil.value(prefix, 0) > QDateTime::currentMSecsSinceEpoch();
}

void markUnreachable(const QString& prefix)
{
    QMutexLocker lock(&g_coolMutex);
    g_coolUntil.insert(prefix, QDateTime::currentMSecsSinceEpoch() + kCoolDownMs);
}

void markReachable(const QString& prefix)
{
    QMutexLocker lock(&g_coolMutex);
    g_coolUntil.remove(prefix);
}

// 是否所有站点都在冷却中。是的话调用方会清空冷却表 —— 否则用户会遇到
// "怎么搜都没结果且无法自行恢复"（冷却表在进程内，只能重启程序）。
bool allCooling()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker lock(&g_coolMutex);
    for (const Site& s : kSites)
        if (g_coolUntil.value(QLatin1String(s.prefix), 0) <= now) return false;
    return true;
}

void clearCooldown()
{
    QMutexLocker lock(&g_coolMutex);
    g_coolUntil.clear();
}

// ---------------------------------------------------------------------------
// 给一次子调用套上「父取消 + 自身超时」的双重取消
// ---------------------------------------------------------------------------
// YtDlpWrapper::search 只认一个 CancelToken，而我们需要的是
// "父搜索被取消" 或 "本站点预算用完" 二者任一都生效。所以起一个看门狗线程，
// 盯着两个条件，命中就把子令牌置位（yt-dlp 进程会在 60ms 内被轮询杀掉）。
struct BudgetedCall
{
    CancelTokenPtr                    token;
    std::thread                       watchdog;
    // 用 shared_ptr 而不是裸 atomic：看门狗线程要长期持有它，而
    // BudgetedCall 本身是按值返回的 —— 捕获局部变量的引用会悬空。
    std::shared_ptr<std::atomic_bool> budgetFired =
        std::make_shared<std::atomic_bool>(false);
};

BudgetedCall withBudget(const CancelTokenPtr& parent, qint64 ms)
{
    BudgetedCall bc;
    bc.token = std::make_shared<CancelToken>();
    const CancelTokenPtr child = bc.token;
    const std::shared_ptr<std::atomic_bool> fired = bc.budgetFired;
    bc.watchdog = std::thread([child, parent, ms, fired]() {
        QElapsedTimer t;
        t.start();
        while (!child->isCanceled()) {
            if (parent && parent->isCanceled()) { child->cancel(); return; }
            if (t.elapsed() >= ms) { fired->store(true); child->cancel(); return; }
            QThread::msleep(40);
        }
    });
    return bc;
}

} // namespace

SourceMeta WebSource::meta() const
{
    SourceMeta m;
    m.id      = QStringLiteral("web");
    m.name    = QStringLiteral("全网搜索");
    m.icon    = QStringLiteral("🌐");
    m.desc    = QStringLiteral("哔哩哔哩 / YouTube / SoundCloud 级联，yt-dlp 解析页面");
    m.kind    = SourceKind::OfficialPreview;
    m.free    = true;              // 页面地址交给 yt-dlp 在下载阶段解析出音频
    m.trust   = 4;
    m.tier    = 1;                 // 第二层：全网兜底
    m.license = QStringLiteral("站点页面");
    return m;
}

bool WebSource::available() const { return YtDlpWrapper::healthy(); }

QString WebSource::unavailableReason() const { return YtDlpWrapper::unavailableReason(); }

QVector<SearchHit> WebSource::search(const SearchContext& ctx)
{
    QVector<SearchHit> out;

    YtDlpWrapper yt;
    if (!yt.available() || !yt.healthy()) {
        qInfo() << "[Web] yt-dlp 不可用:" << yt.unavailableReason();
        return out;
    }

    if (allCooling()) {
        qInfo() << "[Web] 所有站点都在冷却中，清空冷却表重试一次";
        clearCooldown();
    }

    const qint64 budgetMs = ctx.budgetMs > 0 ? ctx.budgetMs : 25000;
    QElapsedTimer clock;
    clock.start();

    // 检索词用 siteSearchText()：主歌手 + 去掉 feat. 后缀的干净歌名。
    //   · searchText() 会把风格关键词（流行/华语/钢琴…）一起拼进来，多出来的词
    //     会把一堆翻唱、合集、试音视频顶到前面；
    //   · platformSearchText() 保留完整的 artist 字段 —— 那对音乐平台是对的
    //     （曲库里歌手就是这么存的），但站点搜索是**全文检索**，多歌手串与分隔符
    //     会立刻变成噪音：实测把 Cartoon / Jéja / Coleman Trapp Why We Lose
    //     原样发过去，返回的是 Pihu parth ki cartoon video（Cartoon 被当卡通）。
    const QString kw = ctx.intent.siteSearchText();
    // 检索词是"搜错东西"类问题的第一现场，必须可观测：一旦它退化成单个词
    // （比如只剩 "Cartoon"），哔哩哔哩会把整页儿童动画塞回来。
    qInfo() << "[Web] 站点检索词:" << kw
            << "| 原始歌手串:" << ctx.intent.artist;
    const QStringList names = splitArtists(ctx.intent.artist);
    if (names.size() > 1)
        qInfo() << "[Web] 歌手切分" << names.size() << "段:" << names.join(QStringLiteral(" | "));

    for (const Site& site : kSites) {
        const QString prefix = QLatin1String(site.prefix);
        const QString label  = QString::fromUtf8(site.label);

        if (ctx.cancel && ctx.cancel->isCanceled()) {
            qInfo() << "[Web] 已被取消，停止站点级联";
            break;
        }
        if (coolingDown(prefix)) {
            qInfo() << "[Web] 跳过" << label << "（该站点不可达，仍在冷却中）";
            continue;
        }
        // 连通性预检：连不上就别起 yt-dlp 了（那要耗 30~44 秒）
        QElapsedTimer probe;
        probe.start();
        if (!hostReachable(QString::fromUtf8(site.host))) {
            qWarning() << "[Web]" << label << "TCP 预检失败（耗时" << probe.elapsed() << "ms）"
                       << "→ 判定不可达，冷却" << (kCoolDownMs / 60000) << "分钟";
            markUnreachable(prefix);
            continue;
        }

        const qint64 left = budgetMs - clock.elapsed();
        if (left < kSiteMinBudgetMs) {
            qInfo() << "[Web] 剩余预算" << left << "ms 不足以再跑一个站点，停止级联";
            break;
        }
        // 首个站点（哔哩哔哩）拿走绝大部分预算；后续站点只在"还没有结果"或
        // "预算还有富余"时给一小段，避免海外站点把整次搜索拖到总超时。
        qint64 siteBudget = out.isEmpty()
                                ? left - 800
                                : qMin<qint64>(left - 1500, kExtraSiteBudgetMs);
        siteBudget = qBound<qint64>(3000, siteBudget, left);

        int limit = site.limit;
        if (ctx.limitPerSource > 0) limit = qMin(limit, ctx.limitPerSource);
        limit = qMax(1, limit);

        QVector<YtDlpWrapper::SearchItem> items;
        QString err;
        const qint64 t0 = clock.elapsed();
        BudgetedCall bc = withBudget(ctx.cancel, siteBudget);
        const bool ok = yt.search(kw, limit, &items, prefix, &err, bc.token, site.flat);
        const bool budgetFired = bc.budgetFired->load();
        bc.token->cancel();                                  // 停掉看门狗
        if (bc.watchdog.joinable()) bc.watchdog.join();
        const qint64 cost = clock.elapsed() - t0;

        if (!ok && items.isEmpty()) {
            if (budgetFired) {
                // 是我们自己的预算掐的，不是站点故障 —— 不能记进冷却表
                qInfo() << "[Web]" << label << "预算" << siteBudget << "ms 用尽，未拿到结果";
            } else {
                qWarning() << "[Web]" << label << "搜索失败:" << err
                           << "→ 冷却" << (kCoolDownMs / 60000) << "分钟";
                markUnreachable(prefix);
            }
            continue;
        }
        if (ok) markReachable(prefix);

        const bool partial = !ok && !items.isEmpty();
        int taken = 0;
        for (const YtDlpWrapper::SearchItem& s : items) {
            if (s.title.isEmpty() || s.pageUrl.isEmpty()) continue;

            SearchHit h;
            h.sourceId    = QStringLiteral("web");
            // 全局 id 带上站点标识：不同站点可能给出同样的条目 id，
            // 撞号会让前端把两条结果当成同一条（去重是按键名，但 id 用于
            // 试听/下载的定位，不能重）。
            h.id          = QStringLiteral("web:") + QLatin1String(site.id)
                            + QLatin1Char(':') + s.id;
            h.title       = s.title;
            // 署名：视频站的歌手名写在标题里，uploader 常是字幕组/搬运号。
            // 直接拿 uploader 当歌手，"原唱"这一项几乎恒为 0 —— 25 分白丢，
            // 实测正确的 YOASOBI《群青》THE FIRST TAKE 版因此只拿 51 分排不上来。
            // 认不出来就老实沿用 uploader（绝不硬填查询歌手）。
            const QString credited = IntentParser::creditedArtist(s.title, s.uploader, ctx.intent);
            h.artist      = credited.isEmpty() ? s.uploader : credited;
            h.album       = label;                 // 来源站点，界面上可见
            h.durationSec = s.durationSec;
            h.quality     = Quality::K128;          // 页面地址，实际音质下载时才定
            h.license     = label + QStringLiteral(" · 站点页面");
            h.free        = true;
            h.previewOnly = false;
            h.streamUrl   = s.pageUrl;              // 页面 URL，不是音频流
            h.downloadUrl = s.pageUrl;
            h.coverUrl    = s.thumbnail;
            // 热度：站点给的播放量对数归一（哔哩哔哩稳定有，flat 模式常没有）
            h.heat        = heatFromCount(s.viewCount);
            h.heatText    = heatTextFromCount(s.viewCount);
            out.append(h);
            ++taken;
        }
        qInfo() << "[Web]" << label << "命中" << taken << "条（耗时" << cost << "ms"
                << (partial ? QStringLiteral("，部分结果") : QString()) << "）";
    }

    qInfo() << "[Web] 全网源合计" << out.size() << "条，耗时" << clock.elapsed() << "ms";
    return out;
}

} // namespace ms
