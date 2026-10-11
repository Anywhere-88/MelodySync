// ============================================================================
//  src/search/sources/JamendoSource.cpp
// ============================================================================
#include "search/sources/JamendoSource.h"

#include "core/HttpClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <QDebug>

namespace ms {

JamendoSource::JamendoSource(const QString& clientId) : m_clientId(clientId) {}

SourceMeta JamendoSource::meta() const
{
    SourceMeta m;
    m.id    = QStringLiteral("jamendo");
    m.name  = QStringLiteral("Jamendo Music");
    m.icon  = QStringLiteral("🎸");
    m.desc  = QStringLiteral("CC 授权，可商用");
    m.kind  = SourceKind::FreeCC;
    m.free  = true;
    m.trust = 5;
    m.license = QStringLiteral("CC BY");
    return m;
}

QVector<SearchHit> JamendoSource::search(const SearchContext& ctx)
{
    QVector<SearchHit> out;
    if (!available()) return out;

    // namesearch 对"歌名"最准；再叠一次 tags 提升风格类查询的召回
    const QString keyword = ctx.intent.name.isEmpty() ? ctx.intent.raw : ctx.intent.name;
    QList<QPair<QString, QString>> params;
    params << qMakePair(QStringLiteral("client_id"), m_clientId)
           << qMakePair(QStringLiteral("format"),    QStringLiteral("json"))
           << qMakePair(QStringLiteral("limit"),     QString::number(ctx.limitPerSource))
           << qMakePair(QStringLiteral("namesearch"), keyword)
           << qMakePair(QStringLiteral("include"),   QStringLiteral("musicinfo+licenses"))
           << qMakePair(QStringLiteral("audioformat"), QStringLiteral("mp32"))
           << qMakePair(QStringLiteral("order"),     QStringLiteral("relevance"));

    const QString url = QStringLiteral("https://api.jamendo.com/v3.0/tracks/?")
                        + HttpClient::buildQuery(params);

    const HttpClient::Response r = HttpClient::getJson(url, {}, 8000);
    if (!r.ok) {
        qInfo() << "[Jamendo] 搜索失败:" << r.error;
        return out;
    }

    const QJsonObject root = r.json();
    const QJsonObject headers = root.value(QStringLiteral("headers")).toObject();
    const int status = headers.value(QStringLiteral("status")).toInt();
    if (status != 0) {
        qInfo() << "[Jamendo] API 返回错误状态:" << status
                << headers.value(QStringLiteral("error_message")).toString();
        return out;
    }

    const QJsonArray items = root.value(QStringLiteral("results")).toArray();
    out.reserve(items.size());

    for (const QJsonValue& v : items) {
        const QJsonObject o = v.toObject();
        SearchHit h;
        h.sourceId    = meta().id;
        h.id          = h.sourceId + QLatin1Char(':') + o.value(QStringLiteral("id")).toString();
        h.title       = o.value(QStringLiteral("name")).toString();
        h.artist      = o.value(QStringLiteral("artist_name")).toString();
        h.album       = o.value(QStringLiteral("album_name")).toString();
        h.durationSec = o.value(QStringLiteral("duration")).toInt();
        h.coverUrl    = o.value(QStringLiteral("image")).toString();
        h.streamUrl   = o.value(QStringLiteral("audio")).toString();
        // audiodownload 只有部分曲目开放（取决于艺术家的授权选择）
        h.downloadUrl = o.value(QStringLiteral("audiodownload")).toString();
        h.free        = !h.downloadUrl.isEmpty();
        h.previewOnly = h.free ? false : true;
        // Jamendo 是否开放下载由艺术家逐个曲目勾选，跟会员/地区都无关。
        // 没有原因说明时界面只能显示一个没有解释的「仅试听」，所以给一句话。
        if (!h.free) h.note = QStringLiteral("艺术家未开放这首的下载");

        // Jamendo 的 audioformat=mp32 只给 128kbps，标 320 不诚信，如实标 128
        h.quality     = h.free ? Quality::K320 : Quality::K128;

        const QString lic = o.value(QStringLiteral("license_ccurl")).toString();
        h.license = lic.isEmpty() ? QStringLiteral("CC BY") : QStringLiteral("CC BY");
        if (lic.contains(QStringLiteral("by-nc")))  h.license = QStringLiteral("CC BY-NC");
        if (lic.contains(QStringLiteral("by-nd")))  h.license = QStringLiteral("CC BY-ND");
        if (lic.contains(QStringLiteral("by-sa")))  h.license = QStringLiteral("CC BY-SA");

        if (h.title.isEmpty()) continue;
        out.append(h);
    }

    qInfo() << "[Jamendo] 命中" << out.size() << "条";
    return out;
}

} // namespace ms
