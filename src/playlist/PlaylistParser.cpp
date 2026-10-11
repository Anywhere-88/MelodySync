// ============================================================================
//  src/playlist/PlaylistParser.cpp
// ============================================================================
#include "playlist/PlaylistParser.h"

#include "core/HttpClient.h"
#include "data/Database.h"
#include "settings/Settings.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

namespace ms {

PlaylistParser::PlaylistParser(QObject* parent) : QObject(parent)
{
    // 恢复上次扫码的登录态。以前 cookie 只活在内存里，程序一退出就没了：
    // 用户下次打开会发现"私有歌单读不了了"，而搜索/下载也拿不到登录态能给的
    // 更高码率 —— 两处都表现为"上次还好好的，这次不行"。
    const QString ck = Settings::instance().neteaseCookie();
    if (!ck.isEmpty()) {
        m_netease.setCookie(ck);
        qInfo() << "[Playlist] 已恢复网易云登录态";
    }
}

QString PlaylistParser::detectPlatform(const QString& url)
{
    if (NeteaseApi::isNeteaseUrl(url)) return QStringLiteral("netease");
    if (QQMusicApi::isQQUrl(url))      return QStringLiteral("qq");
    if (url.contains(QStringLiteral("kugou.com"))) return QStringLiteral("kugou");
    if (url.contains(QStringLiteral("kuwo.cn")))   return QStringLiteral("kuwo");
    return QString();
}

bool PlaylistParser::parse(const QString& url, const QString& platformHint,
                           ParsedPlaylist* out, QString* errorOut)
{
    const QString platform = platformHint.isEmpty() ? detectPlatform(url) : platformHint;

    // 解析链路原来一行日志都没有 —— 出问题时用户只看到"解析出错"，
    // 排查时既不知道走没走到后端、也不知道卡在哪一步。入口和出口各记一条。
    qInfo() << "[Playlist] 解析请求: platform =" << platform << " url =" << url;

    bool ok = false;
    if (platform == QLatin1String("netease"))    ok = m_netease.parsePlaylist(url, out, errorOut);
    else if (platform == QLatin1String("qq"))    ok = m_qq.parsePlaylist(url, out, errorOut);
    else if (platform == QLatin1String("kugou")) ok = parseKugou(url, out, errorOut);
    else if (platform == QLatin1String("kuwo"))  ok = parseKuwo(url, out, errorOut);
    else {
        if (errorOut) *errorOut = QStringLiteral("暂不支持该平台的歌单链接");
        qWarning() << "[Playlist] 无法识别的平台:" << platform << " url =" << url;
        return false;
    }

    if (ok && out) {
        qInfo() << "[Playlist] 成功:" << out->name << "共" << out->songs.size() << "首";
    } else {
        qWarning() << "[Playlist] 失败:" << (errorOut ? *errorOut : QStringLiteral("未知原因"));
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 扫码登录透传
// ---------------------------------------------------------------------------
QJsonObject PlaylistParser::qrCreate(QString* errorOut)
{
    const NeteaseApi::QrSession s = m_netease.createQrSession(errorOut);
    QJsonObject o;
    o[QStringLiteral("ok")]      = !s.unikey.isEmpty();
    o[QStringLiteral("unikey")]  = s.unikey;
    o[QStringLiteral("content")] = s.qrContent;   // 前端用 qrcode.js / 在线渲染二维码
    return o;
}

QJsonObject PlaylistParser::qrPoll(const QString& unikey, QString* errorOut)
{
    QString nickname;
    const int code = m_netease.pollQr(unikey, &nickname, errorOut);

    QJsonObject o;
    o[QStringLiteral("code")]     = code;
    o[QStringLiteral("nickname")] = nickname;
    switch (code) {
    case 800: o[QStringLiteral("status")] = QStringLiteral("expired");  break;
    case 801: o[QStringLiteral("status")] = QStringLiteral("waiting");  break;
    case 802: o[QStringLiteral("status")] = QStringLiteral("scanned");  break;
    case 803: o[QStringLiteral("status")] = QStringLiteral("confirmed");break;
    default:  o[QStringLiteral("status")] = QStringLiteral("error");    break;
    }
    o[QStringLiteral("ok")] = (code == 803);
    if (code == 803) {
        // 落盘：搜索源和下载器都要靠这个 cookie 换更高码率的直链
        Settings::instance().setNeteaseCookie(m_netease.cookie());
        qInfo() << "[Playlist] 网易云登录成功:" << nickname << "（登录态已保存）";
    }
    return o;
}

// ---------------------------------------------------------------------------
// 酷狗：special/song 分页接口
// ---------------------------------------------------------------------------
bool PlaylistParser::parseKugou(const QString& url, ParsedPlaylist* out, QString* errorOut)
{
    static const QRegularExpression re(QStringLiteral("(?:special/single/|specialid=|id=)(\\d{3,})"));
    const QRegularExpressionMatch m = re.match(url);
    if (!m.hasMatch()) {
        if (errorOut) *errorOut = QStringLiteral("无法识别酷狗歌单 ID");
        return false;
    }
    const QString specialId = m.captured(1);

    QList<QPair<QString, QString>> params;
    params << qMakePair(QStringLiteral("specialid"), specialId)
           << qMakePair(QStringLiteral("page"), QStringLiteral("1"))
           << qMakePair(QStringLiteral("pagesize"), QStringLiteral("200"))
           << qMakePair(QStringLiteral("version"), QStringLiteral("9108"));

    const HttpClient::Response r = HttpClient::getJson(
        QStringLiteral("https://mobilecdn.kugou.com/api/v3/special/song?")
            + HttpClient::buildQuery(params),
        { QStringLiteral("Referer: https://www.kugou.com/") }, 18000);
    if (!r.ok) { if (errorOut) *errorOut = r.error; return false; }

    const QJsonObject root = r.json();
    const QJsonArray info = root.value(QStringLiteral("data")).toObject()
                                .value(QStringLiteral("info")).toArray();
    if (info.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("酷狗返回歌单为空");
        return false;
    }

    out->platform   = QStringLiteral("kugou");
    out->sourceUrl  = url;
    out->coverEmoji = QStringLiteral("🏃");
    out->creator    = QStringLiteral("酷狗用户");

    // 歌单名需要通过 detail 接口拿，这里退一步用 ID 命名，避免再多一次请求
    const HttpClient::Response dr = HttpClient::getJson(
        QStringLiteral("https://mobilecdn.kugou.com/api/v3/special/info?specialid=") + specialId,
        { QStringLiteral("Referer: https://www.kugou.com/") }, 12000);
    if (dr.ok) {
        const QJsonObject d = dr.json().value(QStringLiteral("data")).toObject();
        out->name = d.value(QStringLiteral("specialname")).toString();
        const qint64 play = d.value(QStringLiteral("playcount")).toVariant().toLongLong();
        out->playCount = play > 0 ? QStringLiteral("%1万").arg(play / 10000.0, 0, 'f', 1)
                                  : QStringLiteral("—");
    }
    if (out->name.isEmpty()) out->name = QStringLiteral("酷狗歌单 ") + specialId;

    for (const QJsonValue& v : info) {
        const QJsonObject s = v.toObject();
        PlaylistTrack pt;
        pt.title       = s.value(QStringLiteral("songname")).toString();
        pt.durationSec = s.value(QStringLiteral("duration")).toInt();
        pt.album       = s.value(QStringLiteral("album_name")).toString();
        pt.artist      = s.value(QStringLiteral("singername")).toString();
        pt.platformId  = s.value(QStringLiteral("hash")).toString();

        // 酷狗用文件 hash 与"音质标识"表达可用档位，这里按文件名后缀粗略判断
        const QString fname = s.value(QStringLiteral("filename")).toString().toLower();
        pt.bestQuality = fname.contains(QStringLiteral("flac")) ? Quality::Lossless
                                                                 : Quality::K320;
        pt.qualities << QStringLiteral("flac") << QStringLiteral("320") << QStringLiteral("128");
        if (pt.title.isEmpty()) continue;
        out->songs.append(pt);
    }
    return !out->songs.isEmpty();
}

// ---------------------------------------------------------------------------
// 酷我：先取 csrf token + cookie，再请求 playListInfo
// ---------------------------------------------------------------------------
bool PlaylistParser::parseKuwo(const QString& url, ParsedPlaylist* out, QString* errorOut)
{
    static const QRegularExpression re(QStringLiteral("(?:playlist|pid=|/play_detail/)(\\d{3,})"));
    const QRegularExpressionMatch m = re.match(url);
    if (!m.hasMatch()) {
        if (errorOut) *errorOut = QStringLiteral("无法识别酷我歌单 ID");
        return false;
    }
    const QString pid = m.captured(1);

    // 第一步：访问歌单页拿 Csrf Token（酷我接口强校验该头，值必须与 cookie 中的 kw_token 一致）
    if (m_kuwoCsrfToken.isEmpty()) {
        const HttpClient::Response pre = HttpClient::get(
            QStringLiteral("https://www.kuwo.cn/playlist_detail/") + pid,
            { QStringLiteral("Referer: https://www.kuwo.cn/") }, 15000);
        const QString ck = pre.cookieHeader();
        if (!ck.isEmpty()) m_kuwoCookie = ck;
        static const QRegularExpression tk(QStringLiteral("kw_token=([A-Z0-9]+)"));
        const QRegularExpressionMatch tm = tk.match(m_kuwoCookie);
        m_kuwoCsrfToken = tm.hasMatch() ? tm.captured(1) : QStringLiteral("UNKNOWN");
    }

    QList<QPair<QString, QString>> params;
    params << qMakePair(QStringLiteral("pid"), pid)
           << qMakePair(QStringLiteral("pn"), QStringLiteral("1"))
           << qMakePair(QStringLiteral("rn"), QStringLiteral("1000"))
           << qMakePair(QStringLiteral("httpsStatus"), QStringLiteral("1"));

    QStringList headers;
    headers << QStringLiteral("Referer: https://www.kuwo.cn/")
            << (QStringLiteral("csrf: ") + m_kuwoCsrfToken)
            << QStringLiteral("Cookie: kw_token=") + m_kuwoCsrfToken;

    const HttpClient::Response r = HttpClient::getJson(
        QStringLiteral("https://www.kuwo.cn/api/www/playlist/playListInfo?")
            + HttpClient::buildQuery(params), headers, 18000);
    if (!r.ok) { if (errorOut) *errorOut = r.error; return false; }

    const QJsonObject data = r.json().value(QStringLiteral("data")).toObject();
    const QJsonArray list = data.value(QStringLiteral("musicList")).toArray();
    if (list.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("酷我返回歌单为空（可能需要登录或接口已变更）");
        return false;
    }

    out->platform   = QStringLiteral("kuwo");
    out->sourceUrl  = url;
    out->coverEmoji = QStringLiteral("🚗");
    out->name       = data.value(QStringLiteral("name")).toString();
    out->creator    = data.value(QStringLiteral("uname")).toString();
    if (out->name.isEmpty()) out->name = QStringLiteral("酷我歌单 ") + pid;

    for (const QJsonValue& v : list) {
        const QJsonObject s = v.toObject();
        PlaylistTrack pt;
        pt.title       = s.value(QStringLiteral("name")).toString();
        pt.durationSec = s.value(QStringLiteral("duration")).toInt();
        pt.album       = s.value(QStringLiteral("album")).toString();
        pt.artist      = s.value(QStringLiteral("artist")).toString();
        pt.platformId  = s.value(QStringLiteral("rid")).toVariant().toString();
        // 酷我的 hasLossless / hasLossless 字段直接告诉我们有没有无损
        pt.bestQuality = s.value(QStringLiteral("hasLossless")).toBool() ? Quality::Lossless
                                                                          : Quality::K320;
        pt.qualities << QStringLiteral("flac") << QStringLiteral("320");
        if (pt.title.isEmpty()) continue;
        out->songs.append(pt);
    }
    return !out->songs.isEmpty();
}

// ---------------------------------------------------------------------------
qint64 PlaylistParser::saveToLibrary(qint64 userId, const ParsedPlaylist& pl)
{
    Database& db = Database::instance();
    const qint64 pid = db.addPlaylist(userId, pl.name, pl.sourceUrl, pl.platform);
    if (pid <= 0) return -1;
    if (!db.addPlaylistTracks(pid, pl.songs)) {
        db.removePlaylist(userId, pid);
        return -1;
    }
    return pid;
}

} // namespace ms
