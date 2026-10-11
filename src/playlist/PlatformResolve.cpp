// ============================================================================
//  src/playlist/PlatformResolve.cpp
// ============================================================================
#include "playlist/PlatformResolve.h"

#include "core/HttpClient.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

namespace ms {

namespace {

// 网易云：请求 999000 表示"给我能给的最高档"，服务端会回落到实际可用的码率。
// （实测：非无损曲目请求 999000 会返回 br=320000，所以不需要自己逐档探测。）
constexpr int kNeteaseBest = 999000;

// QQ 各档位的文件名前缀（平台自己就是这么拼的）：
//   F000=无损FLAC  A000=APE  M800=320k MP3  M500=128k MP3  C400=128k AAC(m4a)
// 按"无损优先"排列，逐个探测，取第一个平台给了 purl 的。
struct QqVariant
{
    const char* prefix;
    const char* ext;
};
const QqVariant kQqVariants[] = {
    { "F000", "flac" },
    { "A000", "ape"  },
    { "M800", "mp3"  },
    { "M500", "mp3"  },
    { "C400", "m4a"  },
};

QString joinIds(const QStringList& ids)
{
    // 网易云要的是 JSON 数组形态的字符串：[1,2,3]
    QStringList body;
    for (const QString& id : ids) body << id;
    return QLatin1Char('[') + body.join(QLatin1Char(',')) + QLatin1Char(']');
}

} // namespace

// ---------------------------------------------------------------------------
QString DirectAudio::extension() const
{
    if (type == QLatin1String("flac")) return QStringLiteral("flac");
    if (type == QLatin1String("m4a") || type == QLatin1String("aac")) return QStringLiteral("m4a");
    if (type == QLatin1String("mp3")) return QStringLiteral("mp3");
    if (type == QLatin1String("ape")) return QStringLiteral("ape");
    if (type == QLatin1String("wav")) return QStringLiteral("wav");

    // type 缺省时从 URL 路径里猜
    const QString path = QUrl(url).path().toLower();
    for (const char* ext : { "flac", "m4a", "mp3", "aac" })
        if (path.endsWith(QLatin1String(".") + QLatin1String(ext)))
            return QString::fromLatin1(ext);
    return QStringLiteral("mp3");
}

Quality DirectAudio::quality() const
{
    if (type == QLatin1String("flac") || type == QLatin1String("ape")
        || type == QLatin1String("wav"))
        return Quality::Lossless;
    if (br >= 900000) return Quality::Lossless;   // 网易云无损档位上报的 br
    if (br >= 320000) return Quality::K320;
    if (br >= 192000) return Quality::K192;
    if (br > 0)       return Quality::K128;
    return Quality::Unknown;
}

// ---------------------------------------------------------------------------
QString PlatformResolve::platformIdOf(const QString& globalId)
{
    const int i = globalId.indexOf(QLatin1Char(':'));
    return i >= 0 ? globalId.mid(i + 1) : globalId;
}

bool PlatformResolve::supports(const QString& sourceId)
{
    return sourceId == QLatin1String("netease") || sourceId == QLatin1String("qq");
}

QStringList PlatformResolve::headersFor(const QString& sourceId)
{
    // 直链的 Referer 是硬性要求，缺了 CDN 会 403
    if (sourceId == QLatin1String("netease"))
        return { QStringLiteral("Referer: https://music.163.com/") };
    if (sourceId == QLatin1String("qq"))
        return { QStringLiteral("Referer: https://y.qq.com/") };
    return {};
}

// ===========================================================================
//  网易云
// ===========================================================================
QHash<QString, DirectAudio> PlatformResolve::neteaseBatch(const QStringList& songIds,
                                                          const QString& cookie,
                                                          QString* errorOut)
{
    QHash<QString, DirectAudio> out;
    if (songIds.isEmpty()) return out;

    QStringList headers;
    headers << QStringLiteral("Referer: https://music.163.com/");
    if (!cookie.isEmpty()) headers << (QStringLiteral("Cookie: ") + cookie);

    // 分批：URL 长度和单次解析耗时都要控制，80 首一批足够覆盖任何搜索结果页
    const int kBatch = 80;
    for (int i = 0; i < songIds.size(); i += kBatch) {
        const QStringList slice = songIds.mid(i, kBatch);
        const QString url = QStringLiteral("https://music.163.com/api/song/enhance/player/url?ids=")
                            + QUrl::toPercentEncoding(joinIds(slice)) + QStringLiteral("&br=")
                            + QString::number(kNeteaseBest);

        const HttpClient::Response r = HttpClient::getJson(url, headers, 15000);
        if (!r.ok) {
            if (errorOut) *errorOut = r.error;
            qInfo() << "[Netease] 取直链失败:" << r.error;
            continue;
        }

        const QJsonArray data = r.json().value(QStringLiteral("data")).toArray();
        for (const QJsonValue& v : data) {
            const QJsonObject o = v.toObject();
            const QString id = o.value(QStringLiteral("id")).toVariant().toString();

            DirectAudio a;
            a.url       = o.value(QStringLiteral("url")).toString();
            a.br        = o.value(QStringLiteral("br")).toInt();
            a.sizeBytes = o.value(QStringLiteral("size")).toVariant().toLongLong();
            a.type      = o.value(QStringLiteral("type")).toString();
            const int freeTrial = o.value(QStringLiteral("freeTrialInfo")).isNull() ? 0 : 1;

            if (a.url.isEmpty()) {
                // 网易云在拿不到地址时不会给 url，只给一个 code。把常见的两个翻译成人话：
                //   404  —— 本地区无版权 / 已下架
                //   -110 —— 需要会员（这是最常见的那个，实测同一张专辑里
                //           非会员曲目返回正常 url，会员曲目统一回 -110）
                const int code = o.value(QStringLiteral("code")).toInt();
                a.needVip = true;
                if (code == 404)
                    a.error = QStringLiteral("本地区无版权或已下架");
                else if (code == -110)
                    a.error = QStringLiteral("需要网易云会员才能取到全曲");
                else
                    a.error = QStringLiteral("平台未返回播放地址（code=%1）").arg(code);
            } else if (freeTrial) {
                // 平台只给了试听片段：不标记可下载，避免用户下到一个 30 秒文件
                a.ok = false;
                a.needVip = true;
                a.error = QStringLiteral("平台只提供试听片段，需要会员才能下载全曲");
            } else {
                a.ok = true;
            }
            if (!id.isEmpty()) out.insert(id, a);
        }
    }
    return out;
}

DirectAudio PlatformResolve::netease(const QString& songId, const QString& cookie,
                                     QString* errorOut)
{
    const QHash<QString, DirectAudio> m = neteaseBatch({ songId }, cookie, errorOut);
    const auto it = m.constFind(songId);
    if (it != m.constEnd()) return it.value();

    DirectAudio a;
    a.error = QStringLiteral("平台没有返回该曲目的信息");
    return a;
}

// ===========================================================================
//  QQ 音乐
// ===========================================================================
QHash<QString, DirectAudio> PlatformResolve::qqBatch(const QStringList& songMids,
                                                     const QString& cookie,
                                                     QString* errorOut)
{
    QHash<QString, DirectAudio> out;
    if (songMids.isEmpty()) return out;

    // 一次把所有 mid × 所有档位都问一遍：平台只对"真正存在且有权"的组合给 purl。
    // mid 与 filename 是按顺序一一对应的，所以展开后要能反查回 mid。
    QJsonArray midArr, fileArr, typeArr;
    QVector<QString> owner;                    // 第 i 个 filename 属于哪个 mid
    for (const QString& mid : songMids) {
        for (const QqVariant& v : kQqVariants) {
            midArr.append(mid);
            fileArr.append(QString::fromLatin1(v.prefix) + mid + QLatin1Char('.')
                           + QString::fromLatin1(v.ext));
            typeArr.append(0);
            owner.append(mid);
        }
    }

    QJsonObject param;
    param[QStringLiteral("guid")]       = QStringLiteral("10000");
    param[QStringLiteral("songmid")]    = midArr;
    param[QStringLiteral("filename")]   = fileArr;
    param[QStringLiteral("songtype")]   = typeArr;
    param[QStringLiteral("uin")]        = QStringLiteral("0");
    param[QStringLiteral("loginflag")]  = 1;
    param[QStringLiteral("platform")]   = QStringLiteral("20");

    QJsonObject req0;
    req0[QStringLiteral("module")] = QStringLiteral("vkey.GetVkeyServer");
    req0[QStringLiteral("method")] = QStringLiteral("CgiGetVkey");
    req0[QStringLiteral("param")]  = param;

    QJsonObject comm;
    comm[QStringLiteral("uin")]    = 0;
    comm[QStringLiteral("format")] = QStringLiteral("json");
    comm[QStringLiteral("ct")]     = 24;
    comm[QStringLiteral("cv")]     = 0;

    QJsonObject payload;
    payload[QStringLiteral("req_0")] = req0;
    payload[QStringLiteral("comm")]  = comm;

    QStringList headers;
    headers << QStringLiteral("Referer: https://y.qq.com/")
            << QStringLiteral("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                              "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36");
    if (!cookie.isEmpty()) headers << (QStringLiteral("Cookie: ") + cookie);

    // 注意：musicu.fcg 只吃 **裸 JSON body**，不能是表单编码
    const HttpClient::Response r = HttpClient::postRaw(
        QStringLiteral("https://u.y.qq.com/cgi-bin/musicu.fcg"),
        QJsonDocument(payload).toJson(QJsonDocument::Compact),
        QStringLiteral("application/json"), headers, 18000);

    if (!r.ok) {
        if (errorOut) *errorOut = r.error;
        qInfo() << "[QQMusic] 取直链失败:" << r.error;
        return out;
    }

    const QJsonObject data = r.json().value(QStringLiteral("req_0")).toObject()
                                 .value(QStringLiteral("data")).toObject();
    const QJsonArray sips = data.value(QStringLiteral("sip")).toArray();
    QString sip = sips.isEmpty() ? QStringLiteral("http://aqqmusic.tc.qq.com/")
                                 : sips.at(0).toString();
    if (!sip.endsWith(QLatin1Char('/'))) sip += QLatin1Char('/');

    const QJsonArray infos = data.value(QStringLiteral("midurlinfo")).toArray();
    for (int i = 0; i < infos.size() && i < owner.size(); ++i) {
        const QJsonObject m = infos.at(i).toObject();
        const QString mid = owner.at(i);
        const QString purl = m.value(QStringLiteral("purl")).toString();

        DirectAudio& slot = out[mid];        // 同 mid 多次出现：保留第一个成功的
        if (slot.ok) continue;

        const QString file = m.value(QStringLiteral("filename")).toString();
        const QString hint = file.left(4);

        DirectAudio a;
        if (!purl.isEmpty()) {
            a.url  = sip + purl;
            a.type = file.section(QLatin1Char('.'), -1).toLower();
            // 平台返回的 result=0 表示这一档可放；filename 前缀决定码率
            if (hint == QLatin1String("F000") || hint == QLatin1String("A000")) {
                a.br = 999000;
            } else if (hint == QLatin1String("M800")) {
                a.br = 320000;
            } else if (hint == QLatin1String("M500")) {
                a.br = 128000;
            } else {
                a.br = 96000;   // C400 = 128k AAC，按实际听感近似
            }
            a.ok = true;
        } else {
            const int code = m.value(QStringLiteral("result")).toInt();
            a.needVip = (code != 0);
            // 实测码：0 = 这一档可用；104003 = 需要购买/会员；101404 = 该曲目无可放档位。
            // 原先只甩一个 "平台码 104003"，用户看不懂，所以补上人话。
            if (code == 0)
                a.error = QStringLiteral("平台未返回可用地址");
            else if (code == 104003)
                a.error = QStringLiteral("需要 QQ 音乐会员或购买单曲");
            else if (code == 101404)
                a.error = QStringLiteral("该曲目在 QQ 音乐无可播放档位");
            else
                a.error = QStringLiteral("平台拒绝提供地址（码 %1）").arg(code);
        }
        slot = a;
    }

    // 一次请求里连 mid 都没出现的情况也要补上，否则调用方拿不到任何反馈
    for (const QString& mid : songMids) {
        if (!out.contains(mid)) {
            DirectAudio a;
            a.needVip = true;
            a.error = QStringLiteral("平台未返回该曲目的可用地址");
            out.insert(mid, a);
        }
    }
    return out;
}

DirectAudio PlatformResolve::qq(const QString& songMid, const QString& cookie,
                                QString* errorOut)
{
    const QHash<QString, DirectAudio> m = qqBatch({ songMid }, cookie, errorOut);
    const auto it = m.constFind(songMid);
    if (it != m.constEnd()) return it.value();

    DirectAudio a;
    a.error = QStringLiteral("平台没有返回该曲目的信息");
    return a;
}

} // namespace ms
