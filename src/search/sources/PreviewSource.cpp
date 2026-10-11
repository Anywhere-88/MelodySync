// ============================================================================
//  src/search/sources/PreviewSource.cpp
// ============================================================================
#include "search/sources/PreviewSource.h"

#include "core/HttpClient.h"
#include "playlist/PlatformResolve.h"
#include "settings/Settings.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>

namespace ms {

namespace {

SourceMeta makeMeta(const QString& id, const QString& name, const QString& icon,
                    const QString& desc, int trust, bool downloadable,
                    const QString& license)
{
    SourceMeta m;
    m.id    = id;
    m.name  = name;
    m.icon  = icon;
    m.desc  = desc;
    m.kind  = SourceKind::OfficialPreview;
    m.free  = downloadable;      // 前端据此决定标签是"可下载"还是"仅试听"
    m.trust = trust;
    m.tier  = 0;                 // 第一层：平台直连，优先级高于全网源
    m.license = license;
    return m;
}

// ---------------------------------------------------------------------------
// 网易云曲目详情里的两个关键字段
// ---------------------------------------------------------------------------
struct NeteaseExtra
{
    double pop       = -1.0;   // 流行度 0-100（-1 = 没拿到）
    int    coverType = 0;      // 0 未知 / 1 原唱 / 2 翻唱
};

// 搜索接口**不返回**流行度，也不标原唱/翻唱，所以额外打一次 song/detail。
// 这一次请求换来两个关键信号（用户明确要求热度参与排序，而"原唱"是搜歌时
// 最容易踩的坑）：
//   - pop            → 热度
//   - originCoverType → 原唱标记。实测搜"周杰伦 晴天"返回的前 5 条
//                       标题里都写着"原唱 周杰伦"，但 originCoverType 全是 2
//                       （翻唱）—— 光看标题根本分辨不出谁是原唱。
// 拿不到就整体降级：heat 保持 -1（评分取中性值），coverFlag 保持 0（未知），
// 绝不用"猜"来填这些字段。
QHash<QString, NeteaseExtra> fetchNeteaseExtras(const QStringList& ids,
                                                const QString& cookie)
{
    QHash<QString, NeteaseExtra> out;
    if (ids.isEmpty()) return out;

    QStringList parts;
    parts.reserve(ids.size());
    for (const QString& id : ids)
        parts << QStringLiteral("{\"id\":%1}").arg(id);

    QList<QPair<QString, QString>> form;
    form << qMakePair(QStringLiteral("c"), QStringLiteral("[%1]").arg(parts.join(QLatin1Char(','))));

    QStringList headers;
    headers << QStringLiteral("Referer: https://music.163.com/");
    if (!cookie.isEmpty()) headers << (QStringLiteral("Cookie: ") + cookie);

    const HttpClient::Response r = HttpClient::postForm(
        QStringLiteral("https://music.163.com/api/v3/song/detail"), form, headers, 12000);
    if (!r.ok) {
        qInfo() << "[Netease] 取曲目详情失败（热度/原唱标记将缺失）:" << r.error;
        return out;
    }

    for (const QJsonValue& v : r.json().value(QStringLiteral("songs")).toArray()) {
        const QJsonObject o = v.toObject();
        NeteaseExtra e;
        e.pop       = o.value(QStringLiteral("pop")).toDouble(-1.0);
        e.coverType = o.value(QStringLiteral("originCoverType")).toInt(0);
        out.insert(QString::number(o.value(QStringLiteral("id")).toVariant().toLongLong()), e);
    }
    return out;
}

} // namespace

PreviewSource::PreviewSource(const QString& platformId) : m_platform(platformId) {}

QVector<SourceMeta> PreviewSource::allMetas()
{
    return {
        // 这两个源能换到官方音频直链；能不能下取决于平台是否放行该曲目
        makeMeta(QStringLiteral("netease"), QStringLiteral("网易云音乐"), QStringLiteral("🎵"),
                 QStringLiteral("官方直链，会员曲目会如实标记"), 5, true,
                 QStringLiteral("官方直链")),
        makeMeta(QStringLiteral("qq"),      QStringLiteral("QQ 音乐"),  QStringLiteral("🎶"),
                 QStringLiteral("官方直链，付费曲目会如实标记"), 5, true,
                 QStringLiteral("官方直链")),
    };
}

SourceMeta PreviewSource::meta() const
{
    for (const SourceMeta& m : allMetas())
        if (m.id == m_platform) return m;
    return makeMeta(m_platform, m_platform, QStringLiteral("♪"),
                    QStringLiteral("平台源"), 5, true, QStringLiteral("官方直链"));
}

QVector<SearchHit> PreviewSource::search(const SearchContext& ctx)
{
    if (m_platform == QLatin1String("netease")) return searchNetease(ctx);
    if (m_platform == QLatin1String("qq"))      return searchQQMusic(ctx);
    return {};
}

// 平台源走自己的 HTTP 接口，不需要 yt-dlp 之类的任何外部程序
bool PreviewSource::available() const { return true; }

QString PreviewSource::unavailableReason() const { return QString(); }

// ---------------------------------------------------------------------------
// 网易云：搜索拿 songId 列表 → 批量换官方音频直链 + 补齐热度/原唱标记
// ---------------------------------------------------------------------------
QVector<SearchHit> PreviewSource::searchNetease(const SearchContext& ctx)
{
    QVector<SearchHit> out;

    const QString kw = ctx.intent.platformSearchText();
    QList<QPair<QString, QString>> form;
    form << qMakePair(QStringLiteral("s"), kw)
         << qMakePair(QStringLiteral("type"), QStringLiteral("1"))
         << qMakePair(QStringLiteral("offset"), QStringLiteral("0"))
         << qMakePair(QStringLiteral("limit"), QString::number(ctx.limitPerSource))
         << qMakePair(QStringLiteral("total"), QStringLiteral("true"));

    const HttpClient::Response r = HttpClient::postForm(
        QStringLiteral("https://music.163.com/api/search/get/web"),
        form,
        { QStringLiteral("Referer: https://music.163.com/"),
          QStringLiteral("Cookie: appver=2.0.2") },
        15000);
    if (!r.ok) { qInfo() << "[Netease] 搜索失败:" << r.error; return out; }

    const QJsonArray songs = r.json().value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("songs")).toArray();

    // 先把 id 收齐，再一次性换直链/取详情：12 条结果 2 次请求，而不是 24 次
    QStringList ids;
    for (const QJsonValue& v : songs) {
        const QString id = QString::number(v.toObject().value(QStringLiteral("id"))
                                               .toVariant().toLongLong());
        if (!id.isEmpty() && id != QLatin1String("0")) ids << id;
    }
    const QString cookie = Settings::instance().neteaseCookie();
    const QHash<QString, DirectAudio> urls    = PlatformResolve::neteaseBatch(ids, cookie);
    const QHash<QString, NeteaseExtra> extras = fetchNeteaseExtras(ids, cookie);

    for (const QJsonValue& v : songs) {
        const QJsonObject s = v.toObject();
        const QString id = QString::number(s.value(QStringLiteral("id")).toVariant().toLongLong());

        SearchHit h;
        h.sourceId    = QStringLiteral("netease");
        h.id          = h.sourceId + QLatin1Char(':') + id;
        h.title       = s.value(QStringLiteral("name")).toString();
        h.durationSec = s.value(QStringLiteral("duration")).toInt() / 1000;   // 毫秒 → 秒

        const QJsonArray artists = s.value(QStringLiteral("artists")).toArray();
        if (!artists.isEmpty())
            h.artist = artists.at(0).toObject().value(QStringLiteral("name")).toString();
        const QJsonObject album = s.value(QStringLiteral("album")).toObject();
        h.album    = album.value(QStringLiteral("name")).toString();
        h.coverUrl = album.value(QStringLiteral("picUrl")).toString();

        // 热度与原唱标记（拿不到就保持"未知"，见 fetchNeteaseExtras 的说明）
        const NeteaseExtra ex = extras.value(id);
        if (ex.pop >= 0.0) {
            h.heat     = qBound(0.0, ex.pop / 100.0, 1.0);
            h.heatText = QStringLiteral("热度 %1").arg(static_cast<int>(ex.pop));
        }
        h.coverFlag = ex.coverType == 2 ? 2 : (ex.coverType == 1 ? 1 : 0);

        const DirectAudio a = urls.value(id);
        if (a.ok) {
            // 有官方直链：可播也可存
            h.downloadUrl = a.url;
            h.streamUrl   = a.url;
            h.quality     = a.quality();
            h.fileSizeKb  = static_cast<int>(a.sizeBytes / 1024);
            h.free        = true;
            h.previewOnly = false;
            h.license     = QStringLiteral("官方直链");
        } else {
            // 拿不到：保留 outer/url 作为在线试听地址，并说清原因
            h.streamUrl   = QStringLiteral("https://music.163.com/song/media/outer/url?id=%1.mp3").arg(id);
            h.quality     = Quality::K128;
            h.license     = QStringLiteral("官方试听");
            h.free        = false;
            h.previewOnly = true;
            h.note        = a.error.isEmpty() ? QStringLiteral("平台未提供可下载地址") : a.error;
        }

        if (h.title.isEmpty()) continue;
        out.append(h);
    }
    qInfo() << "[Netease] 命中" << out.size() << "条（其中可下载"
            << std::count_if(out.cbegin(), out.cend(),
                             [](const SearchHit& x) { return !x.downloadUrl.isEmpty(); })
            << "条）";
    return out;
}

// ---------------------------------------------------------------------------
// QQ 音乐：搜索拿 songmid → 批量换 vkey 直链
// ---------------------------------------------------------------------------
QVector<SearchHit> PreviewSource::searchQQMusic(const SearchContext& ctx)
{
    QVector<SearchHit> out;

    // 用网页版播放器自己的接口（u.y.qq.com/cgi-bin/musicu.fcg），
    // **不是**老教程里常见的 c.y.qq.com/soso/fcgi-bin/client_search_cp。
    // 原因：2026-09 起那个老接口对未登录请求稳定返回 HTTP 500
    // （同一网络同一天，独立脚本与程序都复现），而下面这个接口正常 ——
    // 症状就是"QQ 源永远 0 条"，日志里只有一句 HTTP 500，极难定位。
    // 换过来之后实测同一个英文查询能返回 15 条（含真正的原唱）。
    QJsonObject param;
    param[QStringLiteral("query")]        = ctx.intent.platformSearchText();
    // QQ 这个接口不认太小的 num_per_page（实测给 3 也回 15 条），
    // 所以自己截断，见下面的 mid 收集与结果循环。
    param[QStringLiteral("num_per_page")] = QString::number(qMax(15, ctx.limitPerSource));
    param[QStringLiteral("page_num")]     = QStringLiteral("1");
    param[QStringLiteral("search_type")]  = QStringLiteral("0");

    QJsonObject req;
    req[QStringLiteral("module")] = QStringLiteral("music.search.SearchCgiService");
    req[QStringLiteral("method")] = QStringLiteral("DoSearchForQQMusicDesktop");
    req[QStringLiteral("param")]  = param;

    QJsonObject comm;
    comm[QStringLiteral("ct")]  = QStringLiteral("19");
    comm[QStringLiteral("cv")]  = QStringLiteral("1859");
    comm[QStringLiteral("uin")] = QStringLiteral("0");

    QJsonObject body;
    body[QStringLiteral("comm")] = comm;
    body[QStringLiteral("req")]  = req;

    const HttpClient::Response r = HttpClient::postJson(
        QStringLiteral("https://u.y.qq.com/cgi-bin/musicu.fcg"), body,
        { QStringLiteral("Referer: https://y.qq.com/") },
        15000);
    if (!r.ok) { qInfo() << "[QQMusic] 搜索失败:" << r.error; return out; }

    // **必须查接口自己的状态码**：QQ 在**请求过频**时返回 HTTP 200，但
    // req.code = 2001、歌曲列表为空。只判 r.ok 会把"被限流"报成"QQ 音乐里
    // 没有这首歌"，用户以为平台没版权，实际等半分钟再搜就有。
    // 实测：同一查询连续打两次即触发；间隔 45 秒后重试恢复 15 条，
    // 且与 headers 无关（只带 Referer / 带 UA / 带 Accept 三种组合表现一致）
    // —— 所以它不是缺 UA 或 Referer，就是频率限制。
    // 配合聚合器里"原语言检索词与主检索词相同时不重试"的改动，
    // 正常一次搜索只打一次这个接口，撞限流的概率大幅下降。
    const int reqCode = r.json().value(QStringLiteral("req")).toObject()
                            .value(QStringLiteral("code")).toInt();
    if (reqCode != 0) {
        qWarning() << "[QQMusic] 接口返回 code" << reqCode
                   << "（请求过频被限流；稍后重试即可，本次不计为 0 条）";
        return out;
    }

    const QJsonArray list = r.json().value(QStringLiteral("req")).toObject()
                                .value(QStringLiteral("data")).toObject()
                                .value(QStringLiteral("body")).toObject()
                                .value(QStringLiteral("song")).toObject()
                                .value(QStringLiteral("list")).toArray();

    // 自己截断到配置的条数（接口不认小分页参数）
    QJsonArray trimmed;
    for (int i = 0; i < list.size() && i < ctx.limitPerSource; ++i)
        trimmed.append(list.at(i));

    QStringList mids;
    for (const QJsonValue& v : trimmed) {
        const QString mid = v.toObject().value(QStringLiteral("mid")).toString();
        if (!mid.isEmpty()) mids << mid;
    }
    const QHash<QString, DirectAudio> urls =
        PlatformResolve::qqBatch(mids, Settings::instance().qqCookie());

    for (const QJsonValue& v : trimmed) {
        const QJsonObject s = v.toObject();
        const QString mid = s.value(QStringLiteral("mid")).toString();

        SearchHit h;
        h.sourceId    = QStringLiteral("qq");
        h.id          = h.sourceId + QLatin1Char(':') + mid;
        h.title       = s.value(QStringLiteral("title")).toString(
                            s.value(QStringLiteral("name")).toString());
        h.durationSec = s.value(QStringLiteral("interval")).toInt();

        const QJsonArray singers = s.value(QStringLiteral("singer")).toArray();
        QStringList names;
        for (const QJsonValue& sv : singers)
            names << sv.toObject().value(QStringLiteral("name")).toString();
        h.artist = names.join(QStringLiteral(" / "));

        h.album = s.value(QStringLiteral("album")).toObject()
                      .value(QStringLiteral("name")).toString();

        // 热度：QQ 这个接口不返回播放量/流行度（hotness 字段是空的展示结构），
        // 所以 heat 保持 -1 —— 评分时取中性值，而不是当成"0 播放"来压它。
        // 源内排名仍然会通过 sourceRank 给它一点相对热度的体现。

        // 搜索结果自带各档位字节数，用于"万一没换成直链"时也给个大小参考
        const QJsonObject file = s.value(QStringLiteral("file")).toObject();
        const auto sizeOf = [&file](const char* k) {
            return file.value(QLatin1String(k)).toVariant().toLongLong();
        };

        const DirectAudio a = urls.value(mid);
        if (a.ok) {
            h.downloadUrl = a.url;
            h.streamUrl   = a.url;
            h.quality     = a.quality();
            h.fileSizeKb  = static_cast<int>(a.sizeBytes / 1024);
            h.free        = true;
            h.previewOnly = false;
            h.license     = QStringLiteral("官方直链");
        } else {
            h.quality     = Quality::K128;
            h.license     = QStringLiteral("官方试听");
            h.free        = false;
            h.previewOnly = true;
            h.note        = a.error.isEmpty() ? QStringLiteral("平台未提供可下载地址") : a.error;
            // 128K 档字节数；新接口的键名带 mp3 后缀，两个都认
            qint64 sz = sizeOf("size_128mp3");
            if (sz <= 0) sz = sizeOf("size_128");
            if (sz > 0) h.fileSizeKb = static_cast<int>(sz / 1024);
        }

        if (h.title.isEmpty()) continue;
        out.append(h);
    }
    qInfo() << "[QQMusic] 命中" << out.size() << "条（其中可下载"
            << std::count_if(out.cbegin(), out.cend(),
                             [](const SearchHit& x) { return !x.downloadUrl.isEmpty(); })
            << "条）";
    return out;
}

} // namespace ms
