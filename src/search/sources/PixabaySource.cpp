// ============================================================================
//  src/search/sources/PixabaySource.cpp
// ============================================================================
#include "search/sources/PixabaySource.h"

#include "core/HttpClient.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

namespace ms {

namespace {
const char* kMusicEndpoint = "https://pixabay.com/api/music/";
}

PixabaySource::PixabaySource(const QString& apiKey) : m_apiKey(apiKey) {}

SourceMeta PixabaySource::meta() const
{
    SourceMeta m;
    m.id    = QStringLiteral("pixabay");
    m.name  = QStringLiteral("Pixabay Music");
    m.icon  = QStringLiteral("🎹");
    m.desc  = QStringLiteral("CC0，无署名要求");
    m.kind  = SourceKind::FreeCC;
    m.free  = true;
    m.trust = 5;
    m.license = QStringLiteral("CC0");
    return m;
}

QVector<SearchHit> PixabaySource::search(const SearchContext& ctx)
{
    QVector<SearchHit> out;

    // 背景音乐场景优先用英文风格词召回（Pixabay 的标签体系以英文为主）
    QString keyword = ctx.intent.name.isEmpty() ? ctx.intent.raw : ctx.intent.name;
    if (ctx.intent.searchMode != QLatin1String("music") && !ctx.intent.keywords.isEmpty())
        keyword = ctx.intent.keywords.join(QLatin1Char(' '));

    QList<QPair<QString, QString>> params;
    params << qMakePair(QStringLiteral("q"), keyword)
           << qMakePair(QStringLiteral("per_page"), QString::number(ctx.limitPerSource))
           << qMakePair(QStringLiteral("order"), QStringLiteral("popular"))
           << qMakePair(QStringLiteral("safesearch"), QStringLiteral("true"));
    if (!m_apiKey.isEmpty()) params << qMakePair(QStringLiteral("key"), m_apiKey);

    const HttpClient::Response r =
        HttpClient::getJson(QString::fromUtf8(kMusicEndpoint) + QLatin1Char('?')
                                + HttpClient::buildQuery(params),
                            { QStringLiteral("Accept: application/json"),
                              QStringLiteral("Referer: https://pixabay.com/music/") },
                            15000);
    if (!r.ok) {
        // 端点变更属预期内的情况，降级为"本源无结果"
        qInfo() << "[Pixabay] 检索不可用（源将跳过）:" << r.error;
        return out;
    }

    const QJsonObject root = r.json();
    const QJsonArray hits = root.contains(QStringLiteral("hits"))
                                ? root.value(QStringLiteral("hits")).toArray()
                                : root.value(QStringLiteral("results")).toArray();

    for (const QJsonValue& v : hits) {
        const QJsonObject o = v.toObject();
        SearchHit h;
        h.sourceId  = meta().id;
        const QString rid = o.value(QStringLiteral("id")).toVariant().toString();
        h.id        = h.sourceId + QLatin1Char(':') + rid;
        h.title     = o.value(QStringLiteral("title")).toString();
        h.artist    = o.value(QStringLiteral("user")).toString(QStringLiteral("Pixabay Artist"));
        h.durationSec = o.value(QStringLiteral("duration")).toInt();
        h.license   = QStringLiteral("CC0");
        h.free      = true;
        h.quality   = Quality::K320;
        h.coverUrl  = o.value(QStringLiteral("cover")).toString();

        // 音频直链字段在不同版本叫法不同，逐个尝试
        for (const char* key : { "audio", "audio_url", "url", "download" }) {
            const QString cand = o.value(QLatin1String(key)).toString();
            if (cand.endsWith(QLatin1String(".mp3"), Qt::CaseInsensitive)) { h.downloadUrl = cand; break; }
        }
        h.streamUrl = h.downloadUrl;
        if (h.title.isEmpty() || h.downloadUrl.isEmpty()) continue;
        out.append(h);
    }

    qInfo() << "[Pixabay] 命中" << out.size() << "条";
    return out;
}

} // namespace ms
