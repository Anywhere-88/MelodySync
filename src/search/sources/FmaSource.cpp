// ============================================================================
//  src/search/sources/FmaSource.cpp
// ============================================================================
#include "search/sources/FmaSource.h"

#include "core/HttpClient.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>

namespace ms {

namespace {
const char* kAdvancedSearch = "https://archive.org/advancedsearch.php";
const char* kMetadataApi    = "https://archive.org/metadata/";
// 只搜 FMA 镜像集合，并限制为音频，避免把图片/文本也捞进来
const char* kCollectionQuery = "(collection:(fma) OR collection:(freemusicarchive)) AND mediatype:(audio)";
}

SourceMeta FmaSource::meta() const
{
    SourceMeta m;
    m.id    = QStringLiteral("fma");
    m.name  = QStringLiteral("Free Music Archive");
    m.icon  = QStringLiteral("📻");
    m.desc  = QStringLiteral("CC 授权，独立音乐");
    m.kind  = SourceKind::FreeCC;
    m.free  = true;
    m.trust = 4;
    m.license = QStringLiteral("CC BY-NC");
    return m;
}

QVector<FmaSource::AudioFile> FmaSource::pickAudioFiles(const QJsonObject& metadata)
{
    QVector<AudioFile> files;
    const QJsonArray arr = metadata.value(QStringLiteral("files")).toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject f = v.toObject();
        const QString name = f.value(QStringLiteral("name")).toString();
        const QString fmt  = f.value(QStringLiteral("format")).toString().toLower();
        if (!name.endsWith(QLatin1String(".flac"), Qt::CaseInsensitive)
            && !name.endsWith(QLatin1String(".mp3"), Qt::CaseInsensitive)
            && !fmt.contains(QLatin1String("flac")) && !fmt.contains(QLatin1String("mp3")))
            continue;

        AudioFile af;
        af.name   = name;
        af.sizeKb = f.value(QStringLiteral("size")).toString().toLongLong() / 1024;
        af.quality = name.endsWith(QLatin1String(".flac"), Qt::CaseInsensitive)
                         ? Quality::Lossless : Quality::K320;
        files.append(af);
    }
    // 无损优先
    std::sort(files.begin(), files.end(), [](const AudioFile& a, const AudioFile& b) {
        return qualityRank(a.quality) > qualityRank(b.quality);
    });
    return files;
}

QVector<SearchHit> FmaSource::search(const SearchContext& ctx)
{
    QVector<SearchHit> out;

    const QString kw = ctx.intent.name.isEmpty() ? ctx.intent.raw : ctx.intent.name;
    QString q = QStringLiteral("(") + QString::fromUtf8(kCollectionQuery) + QStringLiteral(") AND (")
                + QStringLiteral("title:(\"%1\")").arg(kw) + QStringLiteral(")");

    QList<QPair<QString, QString>> params;
    params << qMakePair(QStringLiteral("q"), q)
           << qMakePair(QStringLiteral("rows"), QString::number(ctx.limitPerSource))
           << qMakePair(QStringLiteral("page"), QStringLiteral("1"))
           << qMakePair(QStringLiteral("output"), QStringLiteral("json"))
           << qMakePair(QStringLiteral("fl[]"), QStringLiteral("identifier"))
           << qMakePair(QStringLiteral("fl[]"), QStringLiteral("title"))
           << qMakePair(QStringLiteral("fl[]"), QStringLiteral("creator"))
           << qMakePair(QStringLiteral("fl[]"), QStringLiteral("year"));

    const HttpClient::Response r =
        HttpClient::getJson(QString::fromUtf8(kAdvancedSearch) + QStringLiteral("?") + HttpClient::buildQuery(params),
                            {}, 8000);
    if (!r.ok) {
        qInfo() << "[FMA] 检索失败:" << r.error;
        return out;
    }

    const QJsonArray docs = r.json().value(QStringLiteral("response")).toObject()
                                .value(QStringLiteral("docs")).toArray();

    for (const QJsonValue& v : docs) {
        if (ctx.cancel && ctx.cancel->isCanceled()) break;

        const QJsonObject d = v.toObject();
        const QString identifier = d.value(QStringLiteral("identifier")).toString();
        if (identifier.isEmpty()) continue;

        // 第二步：拿元数据里的实际音频文件（多一次请求，但能拿到真实直链与时长）
        const HttpClient::Response mr =
            HttpClient::getJson(QString::fromUtf8(kMetadataApi) + identifier, {}, 6000);
        if (!mr.ok) continue;

        const QJsonObject meta2 = mr.json();
        const QVector<AudioFile> files = pickAudioFiles(meta2);
        if (files.isEmpty()) continue;

        const AudioFile& best = files.first();

        SearchHit h;
        h.sourceId    = meta().id;
        h.id          = h.sourceId + QLatin1Char(':') + identifier + QLatin1Char('#') + best.name;
        h.title       = d.value(QStringLiteral("title")).toString();
        h.artist      = d.value(QStringLiteral("creator")).toString();
        h.album       = identifier;
        h.quality     = best.quality;
        h.fileSizeKb  = static_cast<int>(best.sizeKb);
        h.license     = QStringLiteral("CC BY-NC");
        h.free        = true;
        h.previewOnly = false;
        // 直链形如 https://archive.org/download/<identifier>/<file>
        h.downloadUrl = QStringLiteral("https://archive.org/download/%1/%2")
                            .arg(QString::fromUtf8(QUrl::toPercentEncoding(identifier)),
                                 QString::fromUtf8(QUrl::toPercentEncoding(best.name)));
        h.streamUrl   = h.downloadUrl;
        // archive.org 元数据里时长字段有时是 "212.34" 这种浮点秒
        h.durationSec = static_cast<int>(meta2.value(QStringLiteral("metadata")).toObject()
                                            .value(QStringLiteral("length")).toString().toDouble());
        if (h.title.isEmpty()) h.title = identifier;
        out.append(h);
    }

    qInfo() << "[FMA] 命中" << out.size() << "条";
    return out;
}

} // namespace ms
