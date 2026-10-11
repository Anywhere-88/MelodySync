// ============================================================================
//  src/search/sources/ArchiveSource.cpp
// ============================================================================
#include "search/sources/ArchiveSource.h"

#include "core/HttpClient.h"

#include <QDebug>
#include <QJsonArray>
#include <QUrl>

#include <algorithm>

namespace ms {

namespace {
const char* kAdvancedSearch = "https://archive.org/advancedsearch.php";
const char* kMetadataApi    = "https://archive.org/metadata/";
// 限定：音频 + (公版标记 或 开放音频合集)
const char* kQuery = "mediatype:(audio) AND (licenseurl:(*publicdomain*) OR "
                     "collection:(opensource_audio) OR collection:(audio_music))";
}

SourceMeta ArchiveSource::meta() const
{
    SourceMeta m;
    m.id    = QStringLiteral("archive");
    m.name  = QStringLiteral("Internet Archive");
    m.icon  = QStringLiteral("🏛");
    m.desc  = QStringLiteral("公版录音，无版权");
    m.kind  = SourceKind::PublicDomain;
    m.free  = true;
    m.trust = 5;
    m.license = QStringLiteral("Public Domain");
    return m;
}

QVector<ArchiveSource::AudioFile> ArchiveSource::pickAudioFiles(const QJsonObject& metadata,
                                                                qint64* durationOut)
{
    QVector<AudioFile> files;
    if (durationOut) *durationOut = 0;

    const QJsonObject md = metadata.value(QStringLiteral("metadata")).toObject();
    if (durationOut) {
        // archive.org 的 length 可能是 "212.34" 或 "3:32"（两种格式都见过）
        const QString len = md.value(QStringLiteral("length")).toString();
        if (len.contains(QLatin1Char(':'))) {
            const QStringList p = len.split(QLatin1Char(':'));
            int sec = 0;
            for (const QString& seg : p) sec = sec * 60 + seg.toInt();
            *durationOut = sec;
        } else {
            *durationOut = static_cast<qint64>(len.toDouble());
        }
    }

    const QJsonArray arr = metadata.value(QStringLiteral("files")).toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject f = v.toObject();
        const QString name = f.value(QStringLiteral("name")).toString();
        const QString fmt  = f.value(QStringLiteral("format")).toString().toLower();
        const bool isFlac = name.endsWith(QLatin1String(".flac"), Qt::CaseInsensitive);
        const bool isMp3  = name.endsWith(QLatin1String(".mp3"), Qt::CaseInsensitive);
        if (!isFlac && !isMp3 && !fmt.contains(QLatin1String("flac")) && !fmt.contains(QLatin1String("mp3")))
            continue;
        // 排除 archive.org 自动生成的衍生副本（*_vbr.mp3 等），它们码率更低
        if (name.contains(QLatin1String("_vbr.")) || name.contains(QLatin1String("_64kb.")))
            continue;

        AudioFile af;
        af.name    = name;
        af.sizeKb  = f.value(QStringLiteral("size")).toString().toLongLong() / 1024;
        af.quality = isFlac ? Quality::Lossless : Quality::K320;
        files.append(af);
    }

    std::sort(files.begin(), files.end(), [](const AudioFile& a, const AudioFile& b) {
        return qualityRank(a.quality) > qualityRank(b.quality);
    });
    return files;
}

QVector<SearchHit> ArchiveSource::search(const SearchContext& ctx)
{
    QVector<SearchHit> out;

    const QString kw = ctx.intent.name.isEmpty() ? ctx.intent.raw : ctx.intent.name;
    QString q = QStringLiteral("(") + QString::fromUtf8(kQuery) + QStringLiteral(") AND ")
                + QStringLiteral("(title:(\"%1\")%2)")
                      .arg(kw,
                           ctx.intent.artist.isEmpty()
                               ? QString()
                               : QStringLiteral(" AND creator:(\"%1\")").arg(ctx.intent.artist));

    QList<QPair<QString, QString>> params;
    params << qMakePair(QStringLiteral("q"), q)
           << qMakePair(QStringLiteral("rows"), QString::number(ctx.limitPerSource))
           << qMakePair(QStringLiteral("output"), QStringLiteral("json"))
           << qMakePair(QStringLiteral("fl[]"), QStringLiteral("identifier"))
           << qMakePair(QStringLiteral("fl[]"), QStringLiteral("title"))
           << qMakePair(QStringLiteral("fl[]"), QStringLiteral("creator"));

    const HttpClient::Response r =
        HttpClient::getJson(QString::fromUtf8(kAdvancedSearch) + QLatin1Char('?')
                                + HttpClient::buildQuery(params), {}, 8000);
    if (!r.ok) {
        qInfo() << "[Archive] 检索失败:" << r.error;
        return out;
    }

    const QJsonArray docs = r.json().value(QStringLiteral("response")).toObject()
                                .value(QStringLiteral("docs")).toArray();

    for (const QJsonValue& v : docs) {
        if (ctx.cancel && ctx.cancel->isCanceled()) break;

        const QJsonObject d = v.toObject();
        const QString identifier = d.value(QStringLiteral("identifier")).toString();
        if (identifier.isEmpty()) continue;

        const HttpClient::Response mr =
            HttpClient::getJson(QString::fromUtf8(kMetadataApi) + identifier, {}, 6000);
        if (!mr.ok) continue;

        qint64 duration = 0;
        const QVector<AudioFile> files = pickAudioFiles(mr.json(), &duration);
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
        h.durationSec = static_cast<int>(duration);
        h.license     = QStringLiteral("Public Domain");
        h.free        = true;
        h.downloadUrl = QStringLiteral("https://archive.org/download/%1/%2")
                            .arg(QString::fromUtf8(QUrl::toPercentEncoding(identifier)),
                                 QString::fromUtf8(QUrl::toPercentEncoding(best.name)));
        h.streamUrl   = h.downloadUrl;
        if (h.title.isEmpty()) h.title = identifier;
        out.append(h);
    }

    qInfo() << "[Archive] 命中" << out.size() << "条";
    return out;
}

} // namespace ms
