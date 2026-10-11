// ============================================================================
//  src/playlist/NeteaseApi.cpp
// ============================================================================
#include "playlist/NeteaseApi.h"

#include "core/HttpClient.h"

#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace ms {

namespace {
// 网易云歌单链接的几种常见形态：
//   https://music.163.com/#/playlist?id=2884035
//   https://music.163.com/playlist?id=2884035&userid=xxx
//   https://y.music.163.com/m/playlist?id=2884035
//   .../discover/toplist?id=3778678
const QRegularExpression& idRe()
{
    static const QRegularExpression re(QStringLiteral("(?:playlist|toplist)[^0-9]*?(\\d{4,})"));
    return re;
}
} // namespace

bool NeteaseApi::isNeteaseUrl(const QString& url)
{
    return url.contains(QStringLiteral("music.163.com"))
        || url.contains(QStringLiteral("163cn.tv"));
}

QString NeteaseApi::extractPlaylistId(const QString& url)
{
    const QRegularExpressionMatch m = idRe().match(url);
    if (m.hasMatch()) return m.captured(1);
    // 兜底：任意位置出现的长数字
    static const QRegularExpression num(QStringLiteral("(\\d{5,})"));
    const QRegularExpressionMatch m2 = num.match(url);
    return m2.hasMatch() ? m2.captured(1) : QString();
}

QStringList headersWithCookie(const QString& cookie)
{
    QStringList h;
    h << QStringLiteral("Referer: https://music.163.com/");
    if (!cookie.isEmpty()) h << (QStringLiteral("Cookie: ") + cookie);
    return h;
}

// ---------------------------------------------------------------------------
Quality NeteaseApi::inferQuality(const QJsonObject& t)
{
    // 网易云的音质字段：hr(Hi-Res) > sq(无损) > h(320k) > m(192k) > l(128k)
    // 高级音质字段在未登录时可能为 null，需要判空而不是判存在。
    auto has = [&t](const char* k) {
        const QJsonValue v = t.value(QLatin1String(k));
        return !v.isNull() && !v.isUndefined() && v.isObject();
    };
    if (has("hr")) return Quality::HiRes;
    if (has("sq")) return Quality::Lossless;
    if (has("h"))  return Quality::K320;
    if (has("m"))  return Quality::K192;
    return Quality::K128;
}

// ---------------------------------------------------------------------------
QJsonObject NeteaseApi::fetchPlaylistJson(const QString& playlistId, QString* errorOut)
{
    // 优先 v6（字段更全），失败则回落到旧版接口
    const QStringList endpoints = {
        QStringLiteral("https://music.163.com/api/v6/playlist/detail?id=%1&n=1000&s=8"),
        QStringLiteral("https://music.163.com/api/playlist/detail?id=%1")
    };

    for (const QString& tpl : endpoints) {
        const QString url = tpl.arg(playlistId);
        const HttpClient::Response r =
            HttpClient::getJson(url, headersWithCookie(m_cookie), 18000);
        if (!r.ok) { if (errorOut) *errorOut = r.error; continue; }

        const QJsonObject o = r.json();
        const int code = o.value(QStringLiteral("code")).toInt(-1);
        if (code != 200) {
            if (errorOut) *errorOut = QStringLiteral("网易云返回 code=%1（歌单可能已删除或需要登录）").arg(code);
            continue;
        }
        const QJsonObject pl = o.value(QStringLiteral("playlist")).toObject();
        if (pl.isEmpty()) { if (errorOut) *errorOut = QStringLiteral("歌单数据为空"); continue; }
        return pl;
    }
    return QJsonObject();
}

bool NeteaseApi::parsePlaylist(const QString& url, ParsedPlaylist* out, QString* errorOut)
{
    if (!isNeteaseUrl(url)) {
        if (errorOut) *errorOut = QStringLiteral("不是网易云歌单链接");
        return false;
    }
    const QString pid = extractPlaylistId(url);
    if (pid.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("无法从链接中识别歌单 ID");
        return false;
    }

    const QJsonObject pl = fetchPlaylistJson(pid, errorOut);
    if (pl.isEmpty()) return false;

    out->platform  = QStringLiteral("netease");
    out->sourceUrl = url;
    out->name      = pl.value(QStringLiteral("name")).toString();
    out->creator   = pl.value(QStringLiteral("creator")).toObject()
                        .value(QStringLiteral("nickname")).toString();
    out->coverEmoji = QStringLiteral("🎧");
    const qint64 pc = pl.value(QStringLiteral("playCount")).toVariant().toLongLong();
    out->playCount = pc >= 10000
        ? QStringLiteral("%1万").arg(pc / 10000.0, 0, 'f', 1)
        : QString::number(pc);

    // 关键：网易云 v6 接口在**未登录**时 tracks 数组只回前 ~10 首的完整对象，
    // 而 trackIds 是全量的。直接读 tracks 就会出现"解析歌单最多十首"。
    // 所以以 trackIds 为准，缺多少就分批调 song/detail 补齐详情。
    const QJsonArray idsArr = pl.value(QStringLiteral("trackIds")).toArray();
    QJsonArray tracks = pl.value(QStringLiteral("tracks")).toArray();
    if (tracks.size() < idsArr.size()) {
        qInfo() << "[Netease] tracks 截断：" << tracks.size() << "/"
                << idsArr.size() << "，批量补拉详情";

        QList<qint64> ids;
        ids.reserve(idsArr.size());
        for (const QJsonValue& v : idsArr) {
            const qint64 id = v.toObject().value(QStringLiteral("id")).toVariant().toLongLong();
            if (id > 0) ids << id;
        }

        // song/detail 单次最多接受约 1000 个 id，200/批足够快也不容易超时。
        // 字段与 v6 tracks 同构（name/ar/al/dt/hr/sq/h/m/l），解析逻辑可复用。
        QHash<qint64, QJsonObject> detail;
        const int kBatch = 200;
        for (int i = 0; i < ids.size(); i += kBatch) {
            QJsonArray c;
            const int end = qMin(i + kBatch, ids.size());
            for (int j = i; j < end; ++j) {
                QJsonObject one;
                one.insert(QStringLiteral("id"), ids.at(j));
                c.append(one);
            }
            QList<QPair<QString, QString>> form;
            form << qMakePair(QStringLiteral("c"),
                QString::fromUtf8(QJsonDocument(c).toJson(QJsonDocument::Compact)));
            const HttpClient::Response r = HttpClient::postForm(
                QStringLiteral("https://music.163.com/api/v3/song/detail"),
                form, headersWithCookie(m_cookie), 20000);
            if (!r.ok) { qWarning() << "[Netease] song/detail 批次失败：" << r.error; continue; }
            const QJsonArray songs = r.json().value(QStringLiteral("songs")).toArray();
            for (const QJsonValue& s : songs) {
                const QJsonObject o = s.toObject();
                detail.insert(o.value(QStringLiteral("id")).toVariant().toLongLong(), o);
            }
        }
        qInfo() << "[Netease] 详情补拉完成：" << detail.size() << "/" << ids.size();

        tracks = QJsonArray();
        for (qint64 id : ids) {
            if (detail.contains(id)) tracks.append(detail.value(id));
        }
    }

    out->songs.reserve(tracks.size());
    for (const QJsonValue& v : tracks) {
        const QJsonObject t = v.toObject();
        PlaylistTrack pt;
        pt.title = t.value(QStringLiteral("name")).toString();
        pt.album = t.value(QStringLiteral("al")).toObject()
                     .value(QStringLiteral("name")).toString();
        pt.durationSec = t.value(QStringLiteral("dt")).toInt() / 1000;
        pt.platformId  = QString::number(t.value(QStringLiteral("id")).toVariant().toLongLong());

        const QJsonArray ars = t.value(QStringLiteral("ar")).toArray();
        QStringList names;
        for (const QJsonValue& a : ars)
            names << a.toObject().value(QStringLiteral("name")).toString();
        pt.artist = names.join(QStringLiteral(" / "));

        pt.bestQuality = inferQuality(t);
        // 按推断出的最高音质，列出所有"大概率可用"的档位，供前端展示
        const int r = qualityRank(pt.bestQuality);
        if (r >= qualityRank(Quality::HiRes))     pt.qualities << QStringLiteral("hires");
        if (r >= qualityRank(Quality::Lossless))  pt.qualities << QStringLiteral("flac");
        if (r >= qualityRank(Quality::K320))      pt.qualities << QStringLiteral("320");
        pt.qualities << QStringLiteral("128");

        if (pt.title.isEmpty()) continue;
        out->songs.append(pt);
    }

    if (out->songs.isEmpty()) {
        if (errorOut) *errorOut = m_cookie.isEmpty()
            ? QStringLiteral("该歌单为空或为私有歌单，请先扫码登录")
            : QStringLiteral("该歌单为空");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 扫码登录
// ---------------------------------------------------------------------------
NeteaseApi::QrSession NeteaseApi::createQrSession(QString* errorOut)
{
    QrSession s;
    const HttpClient::Response r = HttpClient::getJson(
        QStringLiteral("https://music.163.com/api/login/qrcode/unikey?type=1"),
        headersWithCookie(m_cookie), 12000);
    if (!r.ok) { if (errorOut) *errorOut = r.error; return s; }

    const QJsonObject o = r.json();
    s.unikey = o.value(QStringLiteral("unikey")).toString();
    if (s.unikey.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("获取二维码 key 失败");
        return s;
    }
    s.qrContent  = QStringLiteral("https://music.163.com/login?codekey=") + s.unikey;
    s.qrImageUrl = QStringLiteral("https://music.163.com/api/login/qrcode/unikey?type=1");
    return s;
}

int NeteaseApi::pollQr(const QString& unikey, QString* nickname, QString* errorOut)
{
    // 官方轮询接口。code 语义：
    //   800 二维码已过期 / 801 等待扫码 / 802 已扫码待确认 / 803 授权成功（返回 cookie）
    const QString url = QStringLiteral("https://music.163.com/api/login/qrcode/client/login")
                        + QStringLiteral("?key=") + unikey + QStringLiteral("&type=1");
    const HttpClient::Response r = HttpClient::getJson(url, headersWithCookie(m_cookie), 12000);
    if (!r.ok) { if (errorOut) *errorOut = r.error; return -1; }

    const QJsonObject o = r.json();
    const int code = o.value(QStringLiteral("code")).toInt(-1);

    if (code == 803) {
        const QString ck = r.cookieHeader();
        if (!ck.isEmpty()) m_cookie = ck;
        if (nickname) {
            *nickname = o.value(QStringLiteral("nickname")).toString();
            if (nickname->isEmpty())
                *nickname = o.value(QStringLiteral("profile")).toObject()
                                .value(QStringLiteral("nickname")).toString();
        }
    } else if (code == 800) {
        if (errorOut) *errorOut = QStringLiteral("二维码已过期，请刷新");
    }
    return code;
}

} // namespace ms
