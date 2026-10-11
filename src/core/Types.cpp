// ============================================================================
//  src/core/Types.cpp
// ============================================================================
#include "core/Types.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace ms {

// ---------------------------------------------------------------------------
// 音质
// ---------------------------------------------------------------------------
QString qualityKey(Quality q)
{
    switch (q) {
    case Quality::K128:     return QStringLiteral("128");
    case Quality::K192:     return QStringLiteral("192");
    case Quality::K320:     return QStringLiteral("320");
    case Quality::Lossless: return QStringLiteral("flac");
    case Quality::HiRes:    return QStringLiteral("hires");
    default:                return QStringLiteral("unknown");
    }
}

Quality qualityFromKey(const QString& key)
{
    const QString k = key.trimmed().toLower();
    if (k.isEmpty() || k == QLatin1String("unknown")) return Quality::Unknown;
    if (k.contains(QLatin1String("hires")) || k.contains(QLatin1String("hi-res"))
        || k.contains(QLatin1String("24bit")) || k == QLatin1String("hires_lossless"))
        return Quality::HiRes;
    if (k == QLatin1String("flac") || k == QLatin1String("alac") || k == QLatin1String("ape")
        || k.contains(QLatin1String("lossless")) || k.contains(QStringLiteral("无损"))
        || k == QLatin1String("sq") || k == QLatin1String("cd"))
        return Quality::Lossless;
    if (k.contains(QLatin1String("320")) || k == QLatin1String("hq"))
        return Quality::K320;
    if (k.contains(QLatin1String("192")))
        return Quality::K192;
    if (k.contains(QLatin1String("128")) || k == QLatin1String("lq"))
        return Quality::K128;
    return Quality::Unknown;
}

QString qualityLabel(Quality q)
{
    switch (q) {
    case Quality::K128:     return QStringLiteral("128K");
    case Quality::K192:     return QStringLiteral("192K");
    case Quality::K320:     return QStringLiteral("320K");
    case Quality::Lossless: return QStringLiteral("FLAC");
    case Quality::HiRes:    return QStringLiteral("HiRes");
    default:                return QStringLiteral("未知");
    }
}

int qualityRank(Quality q) { return static_cast<int>(q); }

QString statusKey(TaskStatus s)
{
    switch (s) {
    case TaskStatus::Pending:     return QStringLiteral("pending");
    case TaskStatus::Downloading: return QStringLiteral("downloading");
    case TaskStatus::Paused:      return QStringLiteral("paused");
    case TaskStatus::Done:        return QStringLiteral("done");
    case TaskStatus::Failed:      return QStringLiteral("failed");
    case TaskStatus::Canceled:    return QStringLiteral("canceled");
    }
    return QStringLiteral("pending");
}

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------
QJsonArray toJsonArray(const QStringList& list)
{
    QJsonArray a;
    for (const QString& s : list) a.append(s);
    return a;
}

QStringList fromJsonArray(const QJsonArray& arr)
{
    QStringList out;
    out.reserve(arr.size());
    for (const QJsonValue& v : arr) out << v.toString();
    return out;
}

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QString formatBytes(qint64 bytes)
{
    if (bytes <= 0) return QStringLiteral("0 B");
    static const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    return QString::number(v, 'f', i == 0 ? 0 : 1) + QStringLiteral(" ") + QLatin1String(units[i]);
}

QString formatSpeed(qint64 bytesPerSec)
{
    if (bytesPerSec <= 0) return QStringLiteral("—");
    return formatBytes(bytesPerSec) + QStringLiteral("/s");
}

QString sanitizeFileName(const QString& name)
{
    QString s = name;
    // Windows 文件名非法字符： \ / : * ? " < > |
    s.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    s.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]")), QStringLiteral(" "));
    // 去掉首尾点和空格（Windows 不允许）
    s = s.trimmed();
    while (s.endsWith(QLatin1Char('.'))) s.chop(1);
    if (s.size() > 120) s = s.left(120);   // 留出扩展名与路径余量
    if (s.isEmpty()) s = QStringLiteral("untitled");
    return s;
}

// ---------------------------------------------------------------------------
// 字符串相似度：字符二元组 Dice 系数 + 覆盖率加权的包含奖励
//   用于 AI 匹配度评分中的"歌名相似度"一项。
//   选择 Dice 而非编辑距离的原因：
//     1) 对中文分词天然友好（中文按字符切片即可）；
//     2) 对"晴天"vs"晴天 (Live)"这类前后缀差异不敏感；
//     3) 计算量小，可在几百条结果上实时跑。
//
// ★ 归一化规则（整段逻辑的成败都在这上面，踩过两个坑）：
//   · 圆括号 / 方括号里的多是**装饰性补充**（feat. / live / 官方版 / 4K），
//     连内容一起去掉 —— 它们不该参与歌名比对。
//   · 书名号 / 引号里包的**就是歌名本身**（《Checklist》---MAX/Chromeo），
//     所以只去符号、保留内容。曾经把《》也当括号删掉内容，结果"歌名藏在书名号
//     里"的正确版本被清成空串，歌名相似度只剩 2 分，反被同名垃圾曲压到后面 ——
//     这是"换源下错版本"里最隐蔽的一个成因。
//   · 包含奖励的 contains 判断也必须在**归一化之后**做：拿原始串去判
//     （"checklist (feat. chromeo)" vs 标题里的《Checklist》---MAX/Chromeo）
//     永远不成立，而那本来是压倒性的一致信号。
// ---------------------------------------------------------------------------
static QString normalizeName(const QString& raw)
{
    QString s = raw.toLower();
    // 圆/方括号：连内容一起去掉
    s.remove(QRegularExpression(QStringLiteral("[\\[（(【].*?[\\]）)】]")));
    // 其余标点与括号符号：只去符号，保留中间的内容（书名号里是歌名）
    s.remove(QRegularExpression(QStringLiteral(
        "[\\s\\-_.·、，,！!?？?：:'\"“”‘’/|｜《》〈〉「」『』]")));
    return s;
}

static QSet<QString> bigramsOf(const QString& normalized)
{
    QSet<QString> out;
    if (normalized.isEmpty()) return out;
    if (normalized.size() == 1) { out.insert(normalized); return out; }
    for (int i = 0; i + 1 < normalized.size(); ++i)
        out.insert(normalized.mid(i, 2));
    return out;
}

double similarity(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) return 0.0;
    if (a.trimmed().compare(b.trimmed(), Qt::CaseInsensitive) == 0) return 1.0;

    const QString na = normalizeName(a);
    const QString nb = normalizeName(b);
    if (na.isEmpty() || nb.isEmpty()) return 0.0;

    const QSet<QString> ga = bigramsOf(na);
    const QSet<QString> gb = bigramsOf(nb);
    int inter = 0;
    for (const QString& g : ga) if (gb.contains(g)) ++inter;
    double dice = (2.0 * inter) / static_cast<double>(ga.size() + gb.size());

    // 一方完整包含另一方时给奖励（"晴天" ⊂ "周杰伦-晴天"）。
    //
    // ★ 奖励必须按"占比"给，不能一律给 0.92。只要标题里出现歌名字符串就给满分，
    //   等于把"标题党 / 合集 / 营销"这类长标题和干净的官方标题同等对待 ——
    //   这同样是"换源下载货不对版本"的主因。实测三例（走全网换源时的前排）：
    //     · 【中字】【BILLKIN】《CHECKLIST》Official MV  ← 泰国艺人的另一首同名歌
    //     · 【4K60FPS】魔力红Maroon 5《Sugar》甜蜜神曲！祝大家情人节快乐
    //     · Sugar - Maroon 5【Hi-Res】百万级装备试听
    //   三条都因为"标题里有歌名"拿到 0.92，把真正的版本压到后面去。
    //
    //   现在按"短串占长串的比例"给奖励：完全相等时仍是 0.92（"晴天" ⊂
    //   "周杰伦-晴天" 这类正常情况不受影响），歌名被淹没在长标题里时明显降低。
    //   cover=1.0 → 0.92；0.5 → 0.76；0.25 → 0.68；0.15 → 0.65
    if (na.contains(nb) || nb.contains(na)) {
        const bool aLonger = na.size() >= nb.size();
        const double cover = (aLonger ? nb.size() : na.size())
                             / static_cast<double>(aLonger ? na.size() : nb.size());
        dice = std::max(dice, 0.60 + 0.32 * cover);
    }
    return std::min(1.0, dice);
}

// ---------------------------------------------------------------------------
// 歌手串切分
// ---------------------------------------------------------------------------
// 平台把多个歌手塞进一个 artist 字段（网易云/QQ 歌单尤其常见），分隔符没有统一
// 标准：网易云常用 " / "，QQ 用 "、"，英文曲目用 " & " / " x " / "feat."。
// 整串拿去比对会全线失效：
//   · creditedArtist 拿整串在标题里找 —— 标题写的是 "Cartoon,Jéja,Coleman Trapp"，
//     分隔符不同，contains 永远不成立 → 认不出署名 → 原唱分归零；
//   · artistMatch 拿整串和候选的单个名字比 —— "MAX / Chromeo" 对 "MAX" 相似度很低。
// 实测 Why We Lose / Checklist / Sugar 这三首歌的换源候选因此全军覆没。
// 所以凡是要比对歌手的地方都必须先切分。
// 注意 "feat."/"ft."/"with" 后面接的是客串歌手：对"歌手是否对得上"这个判据
// 来说他们同样是这首歌的合法署名，一并保留（"MAX / Chromeo" 两个都要能用）。
QStringList splitArtists(const QString& artists)
{
    static const QRegularExpression sep(QStringLiteral(
        "\\s*(?:[/／、,，;；&＆×·|+])\\s*"
        "|\\s+(?:feat\\.?|ft\\.?|featuring|with|vs\\.?)\\s+"),
        QRegularExpression::CaseInsensitiveOption);

    QStringList out;
    const QStringList parts = artists.split(sep, Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        const QString s = p.trimmed();
        if (s.isEmpty()) continue;
        if (out.contains(s, Qt::CaseInsensitive)) continue;   // 去重（"AC/DC / acdc"）
        out << s;
    }
    return out;
}

QString primaryArtist(const QString& artists)
{
    const QStringList list = splitArtists(artists);
    return list.isEmpty() ? artists.trimmed() : list.first();
}

QString cleanSongName(const QString& name)
{
    QString s = name;
    // 把 "(feat. xxx)" / "【ft. xxx】" 摊平成普通词，而不是整段删掉：
    // 括号本身是搜索噪音，但括号里的合作艺人恰好是最有用的区分信息 ——
    // "Checklist" 这个词太常见，光靠它哔哩哔哩会返回论文 checklist、街舞社的
    // 同名舞曲之类完全不相关的东西。
    static const QRegularExpression featTail(QStringLiteral(
        "\\s*[\\[（(【]\\s*(?:feat\\.?|ft\\.?|featuring|with)\\s+([^\\]）)】]*)[\\]）)】]"),
        QRegularExpression::CaseInsensitiveOption);
    s.replace(featTail, QStringLiteral(" \\1"));
    s = s.simplified();
    return s.isEmpty() ? name.trimmed() : s;
}

// ---------------------------------------------------------------------------
// 热度换算
// ---------------------------------------------------------------------------
double heatFromCount(qint64 count)
{
    if (count <= 0) return -1.0;      // 没有数据 ≠ 没人听，交给调用方取中性值
    // log10(1000)=3 → 0.43；1 万 → 0.57；100 万 → 0.86；1000 万 → 1.0
    const double v = std::log10(static_cast<double>(count)) / 7.0;
    return qBound(0.0, v, 1.0);
}

QString heatTextFromCount(qint64 count)
{
    if (count <= 0) return QString();
    if (count >= 100000000)                                   // 亿
        return QStringLiteral("%1亿播放").arg(count / 100000000.0, 0, 'f', 1);
    if (count >= 10000)                                       // 万
        return QStringLiteral("%1万播放").arg(count / 10000.0, 0, 'f', 1);
    return QStringLiteral("%1 播放").arg(count);
}

// ---------------------------------------------------------------------------
// SearchIntent
// ---------------------------------------------------------------------------
QJsonObject SearchIntent::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("raw")]        = raw;
    o[QStringLiteral("name")]       = name;
    o[QStringLiteral("artist")]     = artist;
    o[QStringLiteral("album")]      = album;
    o[QStringLiteral("version")]    = version.isEmpty() ? QStringLiteral("原版") : version;
    o[QStringLiteral("keywords")]   = toJsonArray(keywords);
    o[QStringLiteral("searchMode")] = searchMode;
    o[QStringLiteral("isNatural")]  = isNatural;
    o[QStringLiteral("confidence")] = confidence;
    o[QStringLiteral("altQueries")] = toJsonArray(altQueries);
    o[QStringLiteral("altNames")]   = toJsonArray(altNames);
    o[QStringLiteral("altArtist")]  = altArtist;
    return o;
}

SearchIntent SearchIntent::fromJson(const QJsonObject& o)
{
    SearchIntent i;
    i.raw        = o.value(QStringLiteral("raw")).toString();
    i.name       = o.value(QStringLiteral("name")).toString();
    i.artist     = o.value(QStringLiteral("artist")).toString();
    i.album      = o.value(QStringLiteral("album")).toString();
    i.version    = o.value(QStringLiteral("version")).toString();
    i.keywords   = fromJsonArray(o.value(QStringLiteral("keywords")).toArray());
    i.searchMode = o.value(QStringLiteral("searchMode")).toString(QStringLiteral("music"));
    i.isNatural  = o.value(QStringLiteral("isNatural")).toBool();
    i.confidence = o.value(QStringLiteral("confidence")).toDouble();
    i.altQueries = fromJsonArray(o.value(QStringLiteral("altQueries")).toArray());
    i.altNames   = fromJsonArray(o.value(QStringLiteral("altNames")).toArray());
    i.altArtist  = o.value(QStringLiteral("altArtist")).toString();
    if (i.version.isEmpty()) i.version = QStringLiteral("原版");
    return i;
}

QString SearchIntent::searchText() const
{
    QStringList parts;
    if (!artist.isEmpty()) parts << artist;
    parts << (name.isEmpty() ? raw : name);
    for (const QString& k : keywords) parts << k;
    return parts.join(QLatin1Char(' '));
}

QString SearchIntent::platformSearchText() const
{
    // 只保留"歌手 + 歌名"：平台是按字符串匹配曲库的，多一个词就多一批噪音。
    // 实测同一查询（网易云 / QQ 都成立）：
    //   "杨丞琳 雨爱 流行 华语" → 12 条里大半是 DJ 版 / 翻唱 / 无演唱者版本
    //   "杨丞琳 雨爱"           → 原唱稳定排在第一
    // 注意顺序是"歌手 歌名"而不是反过来：各平台的搜索都更吃这个顺序。
    QStringList parts;
    if (!artist.isEmpty()) parts << artist;
    const QString songName = name.isEmpty() ? raw : name;
    if (!songName.isEmpty()) parts << songName;
    return parts.join(QLatin1Char(' '));
}

QString SearchIntent::siteSearchText() const
{
    // 站点（yt-dlp）检索是全文搜索，多歌手串与分隔符会立刻变成噪音。实测同一个查询：
    //   "Cartoon / Jéja / Coleman Trapp Why We Lose"
    //     → FUSION CONCEPT TOP 8 : CARTOON VS KEZIAH / Pihu parth ki cartoon video
    //       （Cartoon 被当成"卡通"，返回的是印度卡通片）
    //   "Cartoon Why We Lose"
    //     → 前几条全是这首歌（Why We Lose - Cartoon,Jéja,Coleman Trapp / NCS Release …）
    // 所以只留主歌手；歌名去掉 (feat. xxx) 后缀。
    QStringList parts;
    const QString primary = primaryArtist(artist);
    if (!primary.isEmpty()) parts << primary;
    const QString songName = cleanSongName(name.isEmpty() ? raw : name);
    if (!songName.isEmpty()) parts << songName;
    return parts.join(QLatin1Char(' ')).trimmed();
}

// ---------------------------------------------------------------------------
// SearchHit
// ---------------------------------------------------------------------------
QString SearchHit::durationText() const
{
    const int m = durationSec / 60;
    const int s = durationSec % 60;
    return QStringLiteral("%1:%2")
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

QJsonObject SearchHit::toJson() const
{
    // 键名刻意与前端结果卡片模板对齐（src/ttl/art/qual/size/dur/match/right/free/preview）
    QJsonObject o;
    o[QStringLiteral("id")]        = id;
    o[QStringLiteral("src")]       = sourceId;
    o[QStringLiteral("ttl")]       = title;
    o[QStringLiteral("art")]       = artist;
    o[QStringLiteral("album")]     = album;
    o[QStringLiteral("qual")]      = qualityLabel(quality);
    o[QStringLiteral("qkey")]      = qualityKey(quality);
    o[QStringLiteral("size")]      = fileSizeKb > 0 ? (fileSizeKb / 1024.0) : 0.0;  // MB
    o[QStringLiteral("dur")]       = durationText();
    o[QStringLiteral("durSec")]    = durationSec;
    o[QStringLiteral("match")]     = static_cast<int>(std::lround(score));
    o[QStringLiteral("matchRaw")]  = score;
    o[QStringLiteral("detail")]    = scoreDetail;
    o[QStringLiteral("heat")]      = heat;          // -1 = 无热度信号
    o[QStringLiteral("heatText")]  = heatText;
    o[QStringLiteral("coverFlag")] = coverFlag;
    o[QStringLiteral("right")]     = license;
    o[QStringLiteral("free")]      = free;
    o[QStringLiteral("preview")]   = previewOnly;
    o[QStringLiteral("stream")]    = streamUrl;
    o[QStringLiteral("dl")]        = downloadUrl;
    o[QStringLiteral("cover")]     = coverUrl;
    o[QStringLiteral("note")]      = note;
    o[QStringLiteral("candidates")] = toJsonArray(altSources);
    return o;
}

SearchHit SearchHit::fromJson(const QJsonObject& o)
{
    SearchHit h;
    h.id          = o.value(QStringLiteral("id")).toString();
    h.sourceId    = o.value(QStringLiteral("src")).toString();
    h.title       = o.value(QStringLiteral("ttl")).toString();
    h.artist      = o.value(QStringLiteral("art")).toString();
    h.album       = o.value(QStringLiteral("album")).toString();
    h.quality     = qualityFromKey(o.value(QStringLiteral("qual")).toString());
    h.license     = o.value(QStringLiteral("right")).toString();
    h.free        = o.value(QStringLiteral("free")).toBool(true);
    h.previewOnly = o.value(QStringLiteral("preview")).toBool(false);
    h.streamUrl   = o.value(QStringLiteral("stream")).toString();
    h.downloadUrl = o.value(QStringLiteral("dl")).toString();
    h.note        = o.value(QStringLiteral("note")).toString();
    h.score       = o.value(QStringLiteral("matchRaw")).toDouble();
    h.durationSec = o.value(QStringLiteral("durSec")).toInt();
    h.heat        = o.value(QStringLiteral("heat")).toDouble(-1.0);
    h.heatText    = o.value(QStringLiteral("heatText")).toString();
    h.coverFlag   = o.value(QStringLiteral("coverFlag")).toInt();
    return h;
}

// ---------------------------------------------------------------------------
// PlaylistTrack / ParsedPlaylist
// ---------------------------------------------------------------------------
QJsonObject PlaylistTrack::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("name")]    = title;
    o[QStringLiteral("artist")]  = artist;
    o[QStringLiteral("album")]   = album;
    o[QStringLiteral("durSec")]  = durationSec;
    o[QStringLiteral("dur")]     = QStringLiteral("%1:%2")
        .arg(durationSec / 60, 2, 10, QLatin1Char('0'))
        .arg(durationSec % 60, 2, 10, QLatin1Char('0'));
    o[QStringLiteral("q")]       = qualityKey(bestQuality);
    o[QStringLiteral("qualities")] = toJsonArray(qualities);
    o[QStringLiteral("pid")]     = platformId;
    return o;
}

PlaylistTrack PlaylistTrack::fromJson(const QJsonObject& o)
{
    PlaylistTrack t;
    t.title       = o.value(QStringLiteral("name")).toString();
    t.artist      = o.value(QStringLiteral("artist")).toString();
    t.album       = o.value(QStringLiteral("album")).toString();
    t.durationSec = o.value(QStringLiteral("durSec")).toInt();
    t.platformId  = o.value(QStringLiteral("pid")).toString();
    t.qualities   = fromJsonArray(o.value(QStringLiteral("qualities")).toArray());
    t.bestQuality = qualityFromKey(o.value(QStringLiteral("q")).toString());
    return t;
}

QJsonObject ParsedPlaylist::toJson() const
{
    QJsonArray arr;
    for (const PlaylistTrack& t : songs) arr.append(t.toJson());
    QJsonObject o;
    o[QStringLiteral("ok")]        = true;
    o[QStringLiteral("platform")]  = platform;
    o[QStringLiteral("name")]      = name;
    o[QStringLiteral("creator")]   = creator;
    o[QStringLiteral("cover")]     = coverEmoji;
    o[QStringLiteral("playCount")] = playCount;
    o[QStringLiteral("sourceUrl")] = sourceUrl;
    o[QStringLiteral("count")]     = songs.size();
    o[QStringLiteral("songs")]     = arr;
    return o;
}

// ---------------------------------------------------------------------------
// DownloadTask
// ---------------------------------------------------------------------------
QJsonObject DownloadTask::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("id")]      = id;
    o[QStringLiteral("name")]    = title;
    o[QStringLiteral("artist")]  = artist;
    o[QStringLiteral("album")]   = album;
    o[QStringLiteral("src")]     = sourceId;
    o[QStringLiteral("pid")]     = platformId;
    o[QStringLiteral("q")]       = qualityKey(quality);
    o[QStringLiteral("qual")]    = qualityLabel(quality);
    o[QStringLiteral("status")]  = statusKey(status);
    o[QStringLiteral("progress")] = percent;
    o[QStringLiteral("speed")]   = speedBps;
    o[QStringLiteral("speedText")] = formatSpeed(speedBps);
    o[QStringLiteral("size")]    = totalBytes > 0 ? (totalBytes / 1024.0 / 1024.0) : 0.0;
    o[QStringLiteral("sizeText")] = formatBytes(totalBytes);
    // 下载任务本身不记录时长（解析阶段才知道），但前端队列卡片需要一个
    // 稳定存在的字段，否则模板里会渲染出 undefined。给个占位保持一致。
    o[QStringLiteral("dur")]     = QStringLiteral("--:--");
    o[QStringLiteral("durSec")]  = 0;
    o[QStringLiteral("path")]    = savePath;
    o[QStringLiteral("error")]   = error;
    o[QStringLiteral("retries")] = retries;
    return o;
}

// ---------------------------------------------------------------------------
// LibraryItem / DeviceInfo / UserInfo
// ---------------------------------------------------------------------------
QJsonObject LibraryItem::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("id")]       = id;
    o[QStringLiteral("name")]     = title;
    o[QStringLiteral("title")]    = title;
    o[QStringLiteral("artist")]   = artist;
    o[QStringLiteral("album")]    = album;
    o[QStringLiteral("genre")]    = genre;
    o[QStringLiteral("year")]     = year;
    o[QStringLiteral("trackNo")]  = trackNo;
    o[QStringLiteral("durSec")]   = durationSec;
    o[QStringLiteral("dur")]      = QStringLiteral("%1:%2")
        .arg(durationSec / 60, 2, 10, QLatin1Char('0'))
        .arg(durationSec % 60, 2, 10, QLatin1Char('0'));
    o[QStringLiteral("qual")]     = qualityLabel(quality);
    o[QStringLiteral("qkey")]     = qualityKey(quality);
    o[QStringLiteral("format")]   = format;
    o[QStringLiteral("size")]     = sizeBytes / 1024.0 / 1024.0;
    o[QStringLiteral("sizeText")] = formatBytes(sizeBytes);
    o[QStringLiteral("path")]     = filePath;
    o[QStringLiteral("cover")]    = coverPath;
    o[QStringLiteral("addedAt")]  = addedAt;
    return o;
}

QJsonObject DeviceInfo::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("id")]    = id;
    o[QStringLiteral("name")]  = name;
    o[QStringLiteral("type")]  = type;
    o[QStringLiteral("icon")]  = icon;
    o[QStringLiteral("mtp")]   = mtp;
    o[QStringLiteral("mount")] = mountPath;
    o[QStringLiteral("total")] = totalBytes / 1024.0 / 1024.0 / 1024.0;   // GB，前端直接显示
    o[QStringLiteral("used")]  = (totalBytes - freeBytes) / 1024.0 / 1024.0 / 1024.0;
    o[QStringLiteral("free")]  = freeBytes / 1024.0 / 1024.0 / 1024.0;
    o[QStringLiteral("support")] = toJsonArray(supportedFormats);
    return o;
}

QJsonObject UserInfo::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("id")]         = id;
    o[QStringLiteral("name")]       = name;
    o[QStringLiteral("avatar")]     = avatar;
    o[QStringLiteral("avatarGrad")] = avatarGrad;
    o[QStringLiteral("isCloud")]    = cloud;
    o[QStringLiteral("createdAt")]  = createdAt;
    o[QStringLiteral("lastLogin")]  = lastSeen;
    return o;
}

} // namespace ms
