// ============================================================================
//  src/playlist/QQMusicApi.cpp
// ============================================================================
#include "playlist/QQMusicApi.h"

#include "core/HttpClient.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

namespace ms {

namespace {
// 支持的链接形态：
//   https://y.qq.com/n/ryqq/playlist/7011264344
//   https://i.y.qq.com/n2/m/share/details/taoge.html?id=7011264344
//   https://c6.y.qq.com/base/fcgi-bin/u?__=AbCdEf   （短链，需要跟随跳转）
const QRegularExpression& idRe()
{
    static const QRegularExpression re(
        QStringLiteral("(?:playlist/|taoge\\.html\\?id=|disstid=|id=)(\\d{5,})"));
    return re;
}
} // namespace

bool QQMusicApi::isQQUrl(const QString& url)
{
    return url.contains(QStringLiteral("y.qq.com"))
        || url.contains(QStringLiteral("c.y.qq.com"))
        || url.contains(QStringLiteral("i.y.qq.com"))
        || url.contains(QStringLiteral("c6.y.qq.com"));
}

QString QQMusicApi::extractPlaylistId(const QString& url)
{
    QString u = url;
    // 短链（c6.y.qq.com/base/fcgi-bin/u?__=xxx）无法直接解出 ID，先跟随跳转拿最终地址
    if (u.contains(QStringLiteral("fcgi-bin/u")) || u.contains(QStringLiteral("__="))) {
        const HttpClient::Response r = HttpClient::get(u, {}, 12000);
        // FOLLOWLOCATION 已开启，我们只需要看响应里是否带出了 disstid
        const QString body = r.text();
        const QRegularExpressionMatch m2 = idRe().match(body);
        if (m2.hasMatch()) return m2.captured(1);
    }

    const QRegularExpressionMatch m = idRe().match(u);
    return m.hasMatch() ? m.captured(1) : QString();
}

// ---------------------------------------------------------------------------
Quality QQMusicApi::inferQuality(const QJsonObject& song)
{
    const QJsonObject file = song.value(QStringLiteral("file")).toObject();
    auto sizeOf = [&file](const char* k) {
        return file.value(QLatin1String(k)).toVariant().toLongLong();
    };
    // QQ 音乐的音质档位：hires > flac > 320mp3 > 128mp3
    if (sizeOf("size_hires") > 0) return Quality::HiRes;
    if (sizeOf("size_flac") > 0)  return Quality::Lossless;
    if (sizeOf("size_320mp3") > 0) return Quality::K320;
    if (sizeOf("size_192aac") > 0) return Quality::K192;
    return Quality::K128;
}

// ---------------------------------------------------------------------------
QJsonObject QQMusicApi::fetchPlaylistJson(const QString& dissId, QString* errorOut)
{
    // 网页版接口要求 g_tk 参数，未登录时固定为 5381 即可（官方前端同样是这么传的）
    const QString url = QStringLiteral(
        "https://c.y.qq.com/qzone/fcg-bin/fcg_ucc_getcdinfo_byids_cp.fcg"
        "?type=1&json=1&utf8=1&onlysong=0&new_format=1&disstid=%1"
        "&g_tk=5381&loginUin=0&hostUin=0&inCharset=utf8&outCharset=utf-8"
        "&notice=0&platform=yqq.json&needNewCode=0").arg(dissId);

    QStringList headers;
    headers << QStringLiteral("Referer: https://y.qq.com/");
    if (!m_cookie.isEmpty()) headers << (QStringLiteral("Cookie: ") + m_cookie);

    const HttpClient::Response r = HttpClient::getJson(url, headers, 18000);
    if (!r.ok) { if (errorOut) *errorOut = r.error; return QJsonObject(); }

    const QJsonObject o = r.json();
    const QJsonArray cdlist = o.value(QStringLiteral("cdlist")).toArray();
    if (cdlist.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("未能读取歌单，可能是私有歌单或歌单不存在");
        return QJsonObject();
    }
    return cdlist.at(0).toObject();
}

bool QQMusicApi::parsePlaylist(const QString& url, ParsedPlaylist* out, QString* errorOut)
{
    if (!isQQUrl(url)) {
        if (errorOut) *errorOut = QStringLiteral("不是 QQ 音乐歌单链接");
        return false;
    }
    const QString dissId = extractPlaylistId(url);
    if (dissId.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("无法从链接中识别歌单 ID");
        return false;
    }

    const QJsonObject pl = fetchPlaylistJson(dissId, errorOut);
    if (pl.isEmpty()) return false;

    out->platform   = QStringLiteral("qq");
    out->sourceUrl  = url;
    out->name       = pl.value(QStringLiteral("dissname")).toString();
    out->creator    = pl.value(QStringLiteral("nickname")).toString();
    out->coverEmoji = QStringLiteral("🌙");
    const qint64 vc = pl.value(QStringLiteral("visitnum")).toVariant().toLongLong();
    out->playCount  = vc >= 10000 ? QStringLiteral("%1万").arg(vc / 10000.0, 0, 'f', 1)
                                  : QString::number(vc);

    const QJsonArray songs = pl.value(QStringLiteral("songlist")).toArray();
    out->songs.reserve(songs.size());
    for (const QJsonValue& v : songs) {
        const QJsonObject s = v.toObject();
        PlaylistTrack pt;
        pt.title       = s.value(QStringLiteral("songname")).toString(
                             s.value(QStringLiteral("name")).toString());
        pt.durationSec = s.value(QStringLiteral("interval")).toInt();
        pt.album       = s.value(QStringLiteral("albumname")).toString();

        QStringList singers;
        const QJsonArray sl = s.value(QStringLiteral("singer")).toArray();
        for (const QJsonValue& sv : sl)
            singers << sv.toObject().value(QStringLiteral("name")).toString();
        pt.artist = singers.join(QStringLiteral(" / "));

        pt.platformId  = s.value(QStringLiteral("songmid")).toString();
        pt.bestQuality = inferQuality(s);

        const int rk = qualityRank(pt.bestQuality);
        if (rk >= qualityRank(Quality::HiRes))    pt.qualities << QStringLiteral("hires");
        if (rk >= qualityRank(Quality::Lossless)) pt.qualities << QStringLiteral("flac");
        if (rk >= qualityRank(Quality::K320))     pt.qualities << QStringLiteral("320");
        pt.qualities << QStringLiteral("128");

        if (pt.title.isEmpty()) continue;
        out->songs.append(pt);
    }

    if (out->songs.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("歌单中没有可读取的曲目");
        return false;
    }
    return true;
}

} // namespace ms
