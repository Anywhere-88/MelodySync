// ============================================================================
//  src/core/Types.h
//  MelodySync 公共数据类型定义（全工程唯一的"数据字典"）
//  设计原则：所有跨模块传递的数据都用这里的 POD 结构体，
//            JSON 序列化集中在 toJson/fromJson，避免各处手写 QJsonObject。
// ============================================================================
#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QVector>

namespace ms {

// ---------------------------------------------------------------------------
// 音质：数值越大越好。去重时用它挑"优先无损源"。
// ---------------------------------------------------------------------------
enum class Quality : int {
    Unknown = 0,
    K128    = 1,
    K192    = 2,
    K320    = 3,
    Lossless = 4,      // CD 级无损 FLAC / ALAC
    HiRes    = 5       // 24bit/96kHz 及以上
};

QString qualityKey(Quality q);                  // "128" / "320" / "flac" / "hires"
Quality qualityFromKey(const QString& key);     // 容错解析（含 "FLAC"、"无损" 等写法）
QString qualityLabel(Quality q);                // 界面展示："128K" / "320K" / "FLAC"
int     qualityRank(Quality q);                 // 等于底层枚举值，便于排序

// ---------------------------------------------------------------------------
// 搜索源分类
// ---------------------------------------------------------------------------
enum class SourceKind {
    FreeCC,          // CC / CC0 授权，可自由下载
    PublicDomain,    // 公版录音
    ArtistDirect,    // 艺术家直供（SoundCloud 等）
    OfficialPreview  // 平台官方试听接口，只能试听
};

// 源元信息（前端 SOURCES 数组与后端保持 id 一致）
struct SourceMeta {
    QString id;          // netease / qq / web
    QString name;
    QString icon;
    QString desc;
    SourceKind kind = SourceKind::FreeCC;
    bool   free = true;      // 是否提供下载
    int    trust = 5;        // 源可信度 0-5（保留字段，供设置页展示）
    QString license;
    // 检索层次：0 = 平台直连（网易云 / QQ：元数据准、秒级返回），
    //           1 = 全网兜底（yt-dlp 级联各站点，慢但覆盖广）。
    // 用户要的顺序就是"先平台、再全网"：评分时给 0 层一个小加成，
    // 让两者匹配度相当时平台结果排前面。
    int    tier = 1;
};

// ---------------------------------------------------------------------------
// AI 意图解析结果（LLM 输出 → 结构化）
// ---------------------------------------------------------------------------
struct SearchIntent {
    QString raw;                    // 用户原始输入
    QString name;                   // 歌名
    QString artist;                 // 歌手
    QString album;                  // 专辑
    QString version;                // 原版 / 现场版 / 翻唱 / Remix / 伴奏
    QStringList keywords;           // 风格关键词
    QString searchMode = "music";   // music / background / sfx
    bool   isNatural = false;       // 是否自然语言查询
    double confidence = 0.0;        // LLM 自评置信度
    // 原语言变体（LLM 生成，仅当歌曲原名与用户输入语言不同时非空）。
    // 典型场景：输入中文"偶像 YOASOBI"，原唱是日文《アイドル》——
    // 平台按字符串匹配曲库，中文查询永远搜不到日文原唱。
    //   altQueries = 原语言完整检索词（含歌手），聚合器在主查询命中过少时重试用
    //   altNames   = 原语言歌名，评分时与查询名取最高相似度
    //   altArtist  = 原语言歌手名，同理
    QStringList altQueries;
    QStringList altNames;
    QString     altArtist;

    QJsonObject toJson() const;
    static SearchIntent fromJson(const QJsonObject& o);
    QString searchText() const;     // 拼给各源检索用的文本

    // 面向音乐平台的检索词：只留"歌手 + 歌名"。
    // searchText() 会把风格关键词一起拼进去（"杨丞琳 雨爱 流行 华语"）——
    // 对 Jamendo/Pixabay 这类按标签检索的 CC 源是加分项；
    // 但对网易云 / QQ 这种"按字符串在曲库里找"的平台是纯噪音：多出来的词会把
    // 一堆翻唱、DJ 版、伴奏顶到前面，原唱反而被挤出结果页。
    QString platformSearchText() const;

    // 面向**视频站点**（哔哩哔哩 / YouTube / SoundCloud，即 yt-dlp 检索）的检索词。
    // 与 platformSearchText() 的区别在于"歌手的取法"：
    //   platformSearchText() 原样用 artist —— 音乐平台的曲库里歌手字段就是这么存的，
    //     原样查命中率最高（"Cartoon / Jéja / Coleman Trapp" 在网易云就是一行记录）。
    //   站点搜索是**全文检索**，多余的歌手名与分隔符会立刻变成噪音：实测同一个查询
    //     "Cartoon / Jéja / Coleman Trapp Why We Lose"
    //   返回到的前几条是 "FUSION CONCEPT TOP 8 : CARTOON VS KEZIAH"、
    //     "Pihu parth ki cartoon video"（20 秒的印度卡通片）。
    //     换成 "Cartoon Why We Lose" 后前几条才全是这首歌。
    // 所以站点检索只留主歌手，并去掉歌名里的 (feat. xxx) 后缀（它只会引来
    // 一批"feat 版/伴奏版"，而主标题才是检索主体）。
    QString siteSearchText() const;
};

// ---------------------------------------------------------------------------
// 聚合搜索结果（一条 = 某源上的一个候选）
// ---------------------------------------------------------------------------
struct SearchHit {
    QString id;             // 全局唯一：源id + ":" + 源内id
    QString sourceId;
    QString title;
    QString artist;
    QString album;
    int     durationSec = 0;
    Quality quality = Quality::Unknown;
    QString license;        // CC BY / Public Domain / 官方试听 ...
    QString streamUrl;      // 试听 / 播放地址（可播但不一定能存）
    QString downloadUrl;    // 直链下载地址（有值才显示"下载"按钮）
    QString coverUrl;
    int     fileSizeKb = 0;
    bool    free = true;
    bool    previewOnly = false;
    // 为什么这条不可下载：给界面一句人话（"需要会员""平台未返回地址"…）。
    // 没有它的时候，用户只看到一个灰掉的按钮，完全不知道卡在哪一步。
    QString note;
    double  score = 0.0;    // AI 匹配度 0-100
    QString scoreDetail;    // "名称92 歌手100 时长88 源5" —— 便于前端/日志排查
    QStringList altSources; // 去重后被合并掉的其它源 id（同曲多源）
    // 该条在自己源内结果里的原始排名（0 起）。平台搜索结果顺序本身反映
    // 热度/相关度，评分时按此给一点微调，同分条目热度高的排前。
    int     sourceRank = -1;

    // ---- 以下三项是"热度 + 原唱"判定的数据基础，见 IntentParser::scoreOne ----
    // 归一化热度 0..1（各源按自己的量纲换算：网易云 pop/100、
    // 哔哩哔哩/YouTube 播放量对数归一）。**-1 表示该源没给任何热度信号**，
    // 评分时取中性值，绝不当成"0 播放"来惩罚。
    // 为什么要它：同名翻唱满天飞时，播放量/流行度是区分"原唱或热门版本"和
    // "某个 UP 主随手传的"最有效的信号，也是用户明确要求纳入排序的因子。
    double  heat = -1.0;
    QString heatText;       // 展示用："989.7万播放" / "热度 100"
    // 平台给出的原唱标记：0 = 未知 / 1 = 平台确认原唱 / 2 = 平台确认翻唱。
    // 网易云 song/detail 的 originCoverType 直接给这个 —— 实测搜"周杰伦 晴天"
    // 返回的 5 条全是 originCoverType=2 的伪原唱（标题写着"原唱 周杰伦"，
    // 歌手却是别人），靠标题关键词猜根本猜不出来。
    int     coverFlag = 0;

    QJsonObject toJson() const;               // 直接给前端用（键名与前端模板对齐）
    static SearchHit fromJson(const QJsonObject&);
    QString durationText() const;             // "04:29"
};

// ---------------------------------------------------------------------------
// 歌单
// ---------------------------------------------------------------------------
struct PlaylistTrack {
    QString title;
    QString artist;
    QString album;
    int     durationSec = 0;
    QString platformId;
    Quality bestQuality = Quality::Unknown;
    QStringList qualities;
    QJsonObject toJson() const;
    static PlaylistTrack fromJson(const QJsonObject&);
};

struct ParsedPlaylist {
    QString platform;       // netease / qq / kugou / kuwo
    QString name;
    QString creator;
    QString coverEmoji;
    QString playCount;
    QString sourceUrl;
    QVector<PlaylistTrack> songs;
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// 下载任务
// ---------------------------------------------------------------------------
enum class TaskStatus { Pending, Downloading, Paused, Done, Failed, Canceled };
QString statusKey(TaskStatus s);

struct DownloadTask {
    QString id;
    QString title;
    QString artist;
    QString album;
    QString sourceId;
    // 平台内的曲目 id（网易云 songId / QQ songmid）。
    // 有了它，下载时才能**重新**去平台换一次直链 —— 平台直链带时效（网易云
    // 1200 秒过期），排队等十几分钟再下载就会 403，所以必须重解析而不是存地址。
    QString platformId;
    Quality quality = Quality::K320;
    // 期望时长（秒）。换源兜底时用来核对候选是不是同一首歌 ——
    // 时长差太多的候选（30 秒试听片段、一小时循环合集）直接被闸门压掉，
    // 这是"下错版本"的最后一道防线。
    int     durationSec = 0;
    QString url;                 // 直链（免费源）/ 平台页面 URL（走 yt-dlp）/ 空
    QString searchText;          // yt-dlp 检索关键词
    QString coverUrl;            // 封面地址（写入嵌入用）
    QString savePath;
    TaskStatus status = TaskStatus::Pending;
    int     percent = 0;
    qint64  speedBps = 0;
    qint64  totalBytes = 0;
    int     retries = 0;
    QString error;

    QJsonObject toJson() const;      // 字段名与前端 renderQueue 对齐
};

// ---------------------------------------------------------------------------
// 音乐库条目
// ---------------------------------------------------------------------------
struct LibraryItem {
    qint64  id = 0;
    QString title, artist, album, genre, filePath, coverPath;
    int     year = 0;
    int     trackNo = 0;
    int     durationSec = 0;
    Quality quality = Quality::Unknown;
    QString format;          // flac / mp3 / m4a
    qint64  sizeBytes = 0;
    QString addedAt;
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// 设备
// ---------------------------------------------------------------------------
struct DeviceInfo {
    QString id;              // 卷序列号 或 MTP 设备 ID
    QString name;
    QString type;            // "USB 存储" / "MP3 播放器 · MTP" ...
    QString icon;            // emoji
    bool    mtp = false;     // true=MTP, false=USB Mass Storage
    QString mountPath;       // USB 盘符根目录（MTP 为空）
    qint64  totalBytes = 0;
    qint64  freeBytes = 0;
    QStringList supportedFormats;   // 设备能吃哪些格式（决定是否要实时转码）
    QJsonObject toJson() const;
};

struct UserInfo {
    qint64  id = 0;
    QString name;
    QString avatar;
    QString avatarGrad;
    bool    cloud = false;
    QString createdAt;
    QString lastSeen;
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
QJsonArray  toJsonArray(const QStringList& list);
QStringList fromJsonArray(const QJsonArray& arr);
QString     nowIso();
QString     formatBytes(qint64 bytes);
QString     formatSpeed(qint64 bytesPerSec);
QString     sanitizeFileName(const QString& name);   // 去掉 Windows 非法字符
double      similarity(const QString& a, const QString& b);  // 0..1，用于 AI 评分

// 歌手串 → 单个歌手名列表。
// 平台的 artist 字段常把多个歌手用一个字段装下（网易云/QQ 歌单尤其常见）：
//     "Cartoon / Jéja / Coleman Trapp"
//     "MAX / Chromeo"
//     "Martin Garrix & Bebe Rexha"
//     "The Chainsmokers feat. Halsey"
// 整串拿去比对会全线失效：标题里写的是 "Cartoon,Jéja,Coleman Trapp"（分隔符不同），
// 整串 contains 永远不成立；而候选署名只有其中一个名字时也永远对不上。
// 所以凡是要"比对歌手"的地方都必须先切分（见 IntentParser 的 artistMatch /
// creditedArtist，以及 dedupeKey）。
QStringList splitArtists(const QString& artists);

// 拼给"视频站点检索"用的文本：只用主歌手 + 去掉 feat./括号后缀的干净歌名。
// 不能直接把多歌手串塞进去 —— 实测把 "Cartoon / Jéja / Coleman Trapp Why We Lose"
// 原样发给哔哩哔哩，返回的是 "Pihu parth ki cartoon video" 这类印度卡通视频
// （Cartoon 被当成"卡通"），而且斜杠本身会稀释检索词权重。
QString     primaryArtist(const QString& artists);   // 取第一个歌手名
QString     cleanSongName(const QString& name);      // 去掉 feat./ft./括号后缀

// 播放量 → 0..1 热度（对数归一：1 万次 ≈ 0.57，1000 万次 ≈ 1.0）。
// 用对数而不是线性：播放量的分布跨越 5 个数量级，线性归一的话
// 100 万和 1000 万都会贴到 1.0，几千播放的全挤在 0 —— 等于没有区分度。
double      heatFromCount(qint64 count);
QString     heatTextFromCount(qint64 count);   // 12345 → "1.2万播放"

} // namespace ms
