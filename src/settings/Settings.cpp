// ============================================================================
//  src/settings/Settings.cpp
// ============================================================================
#include "settings/Settings.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QStandardPaths>

namespace ms {

namespace {
// 默认启用的搜索源：**只有全网一个**。
//
// 为什么把网易云 / QQ 从默认列表里拿掉（2026-09-23）：
//   平台直连源的优点是"元数据准 + 免费曲目秒下"，但它有一个用户无法接受的
//   副作用 —— **平台上最像原唱的那一条，往往恰好是下不了的**（会员曲/无版权），
//   而次优的同名翻唱、合集却能下载、分数也不低。用户搜一首歌，看到的头条是
//   "原唱 · 仅试听"，往下全是翻唱，于是感受就是"搜出来的原唱下不了，其他结果
//   也没有原唱"。对一个下载器来说，**"搜到的都能下"比"元数据更准"重要**。
//
//   改成"仅全网"之后，搜索返回的每一条都是可下载的页面地址（yt-dlp 去取音频），
//   "仅试听"这个状态在默认配置下不再出现。搜索与换源从此共用同一套口径。
//
// 平台源并没有被删：它们照常注册（SearchAggregator::registerDefaults），
// 设置页里仍能勾选，勾上就恢复"先平台、再全网"的两层检索。老配置里已勾选的
// netease/qq 会在 migrate() 的 v4 迁移里被移除（见那里的说明）。
//
// 老配置里的 jamendo/fma/archive/pixabay/ytmusic/soundcloud/bilibili 同样会在
// migrate() 里被迁移掉，不会变成"勾了但没有的源"。
const QString kDefaultSources = QStringLiteral("web");

// 各 AI 服务商的默认接口地址与模型名。
//
// 为什么需要这张表：设置页只让用户选「服务商」+ 填 Key，baseUrl / model
// 都不是必填项。而 QSettings 里这两项只要没被显式写过，value() 就会回落到
// 写死的 OpenAI 默认值 —— 于是用户选了「智谱 GLM」，请求却仍然发往
// api.openai.com 并带上 gpt-4o-mini 这个模型名，结果是 401/404，
// 现象正好就是「我填了 API 却没用」。
//
// 所以默认值必须跟着 provider 走：用户没显式指定时才用这里的值。
struct ProviderDefaults
{
    QString baseUrl;
    QString model;
};

ProviderDefaults defaultsForProvider(const QString& provider)
{
    const QString p = provider.trimmed().toLower();
    if (p == QLatin1String("glm") || p == QLatin1String("zhipu") || p == QLatin1String("bigmodel"))
        return { QStringLiteral("https://open.bigmodel.cn/api/paas/v4"), QStringLiteral("glm-4-flash") };
    if (p == QLatin1String("claude") || p == QLatin1String("anthropic"))
        return { QStringLiteral("https://api.anthropic.com/v1"), QStringLiteral("claude-3-5-sonnet-latest") };
    if (p == QLatin1String("openai"))
        return { QStringLiteral("https://api.openai.com/v1"), QStringLiteral("gpt-4o-mini") };
    // custom / local：不猜，留空由用户自己填
    return { QString(), QString() };
}
} // namespace

Settings& Settings::instance()
{
    static Settings s;
    return s;
}

Settings::Settings()
    : m_s(QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
              .filePath(QStringLiteral("settings.ini")),
          QSettings::IniFormat)
{
}

// ---------------------------------------------------------------------------
QString Settings::downloadDir() const
{
    QString def = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (def.isEmpty()) def = QDir::homePath() + QStringLiteral("/Music");
    def = QDir(def).filePath(QStringLiteral("MelodySync"));
    return m_s.value(QStringLiteral("download/dir"), def).toString();
}

void Settings::setDownloadDir(const QString& dir) { m_s.setValue(QStringLiteral("download/dir"), dir); }

int Settings::concurrent() const
{
    // 约束在 1..8：低于 1 无意义，高于 8 容易触发平台限流
    return qBound(1, m_s.value(QStringLiteral("download/concurrent"), 3).toInt(), 8);
}

void Settings::setConcurrent(int n) { m_s.setValue(QStringLiteral("download/concurrent"), qBound(1, n, 8)); }

int Settings::retries() const
{
    return qBound(0, m_s.value(QStringLiteral("download/retries"), 2).toInt(), 5);
}

void Settings::setRetries(int n) { m_s.setValue(QStringLiteral("download/retries"), qBound(0, n, 5)); }

QString Settings::preferQuality() const
{
    return m_s.value(QStringLiteral("download/quality"), QStringLiteral("flac")).toString();
}

bool Settings::writeMetadata() const
{
    return m_s.value(QStringLiteral("meta/write"), true).toBool();
}

bool Settings::embedCover() const
{
    return m_s.value(QStringLiteral("meta/cover"), true).toBool();
}

bool Settings::fetchLyrics() const
{
    return m_s.value(QStringLiteral("meta/lyrics"), false).toBool();
}

QString Settings::lyricsDir() const
{
    return m_s.value(QStringLiteral("meta/lyricsDir"), downloadDir()).toString();
}

// ---------------------------------------------------------------------------
QString Settings::aiBaseUrl() const
{
    // 用户显式配置过就用用户的；否则按当前服务商推导（见 defaultsForProvider）
    const QString explicitUrl = m_s.value(QStringLiteral("ai/baseUrl")).toString().trimmed();
    if (!explicitUrl.isEmpty()) return explicitUrl;
    return defaultsForProvider(aiProvider()).baseUrl;
}

QString Settings::aiApiKey() const
{
    return m_s.value(QStringLiteral("ai/apiKey")).toString();
}

QString Settings::aiModel() const
{
    // 同上：空 = 没配置过，跟随服务商取默认模型名
    const QString explicitModel = m_s.value(QStringLiteral("ai/model")).toString().trimmed();
    if (!explicitModel.isEmpty()) return explicitModel;
    return defaultsForProvider(aiProvider()).model;
}

QString Settings::aiProvider() const
{
    return m_s.value(QStringLiteral("ai/provider"), QStringLiteral("openai")).toString();
}

int Settings::aiTimeoutMs() const
{
    return qBound(3000, m_s.value(QStringLiteral("ai/timeout"), 20000).toInt(), 120000);
}

bool Settings::aiThinkingVisible() const
{
    return m_s.value(QStringLiteral("ai/thinking"), true).toBool();
}

// ---------------------------------------------------------------------------
QString Settings::leanCloudAppId() const
{
    return m_s.value(QStringLiteral("cloud/lcAppId"), QString()).toString().trimmed();
}

QString Settings::leanCloudAppKey() const
{
    return m_s.value(QStringLiteral("cloud/lcAppKey"), QString()).toString().trimmed();
}

QString Settings::leanCloudApiBase() const
{
    return m_s.value(QStringLiteral("cloud/lcApiBase"), QString()).toString().trimmed();
}

bool Settings::cloudEnabled() const
{
    return m_s.value(QStringLiteral("cloud/enabled"), true).toBool();
}

// ---------------------------------------------------------------------------
QString Settings::theme() const
{
    return m_s.value(QStringLiteral("ui/theme"), QStringLiteral("dark")).toString();
}

void Settings::setTheme(const QString& t) { m_s.setValue(QStringLiteral("ui/theme"), t); }

QStringList Settings::enabledSources() const
{
    // ⚠ QSettings 的 ini 后端会把**含逗号的值自动反序列化成 QStringList**。
    // 我们自己写的时候是 `setValue("search/sources", "a,b,c")` —— 一个带逗号的
    // QString，QSettings 存盘时会自动加引号写成 `sources="a,b,c"`，读回来还是
    // 字符串，一切正常。
    // 但只要用户手工把引号去掉（`sources=a,b,c`，看起来更自然），读回来就变成
    // QStringList，此时 `.toString()` 得到**空串** → 源列表为空 →
    // 聚合器认为"没配置"，于是把所有源都跑一遍。
    // 这个失败非常安静：没有报错，只是搜索变慢、结果变杂，几乎不可能联想到 ini 的引号。
    // 所以两种类型都要认。
    const QVariant v = m_s.value(QStringLiteral("search/sources"), kDefaultSources);
    QStringList list;
    if (v.metaType().id() == QMetaType::QStringList) {
        list = v.toStringList();
        qInfo() << "[Settings] search/sources 被读成列表（ini 里没加引号），已兼容处理";
    } else {
        list = v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    // 去掉可能存在的空白（用户手写时会带空格）
    for (QString& s : list) s = s.trimmed();
    list.removeAll(QString());
    return list;
}

void Settings::migrate()
{
    // 新增的源不会自己出现在用户已有的配置里：老用户的 search/sources 早就
    // 落盘成一串固定 id，新源对他来说是"从来没见过、也没法勾"的状态，
    // 而聚合器只跑"配置里有的源"——于是新功能对老用户等于不存在。
    //
    // 为什么单独做成一个方法而不是塞在 enabledSources() 里：
    // enabledSources() 是 const 的读取接口，在里面写盘既反直觉，
    // 又会在多线程读配置时产生并发写（搜索工作线程也会读 Settings）。
    // 迁移只该在启动时做一次。
    //
    // v3：搜索源从 8 个收敛成 2 层 3 个。
    //   - jamendo/fma/archive/pixabay 全部下线（国内要么连不上、要么要自己
    //     申请 client_id，勾了等于没勾）；
    //   - ytmusic/soundcloud/bilibili 三个 yt-dlp 源合并成一个 "web"（全网）。
    //   **必须迁移**：聚合器只跑"配置里存在的源"，老配置里全是已下线的 id，
    //   迁移前的结果是"没有启用任何搜索源"——搜索直接罢工，而用户完全不知道
    //   该去设置里改什么。
    //
    // v4（本次）：**搜索默认改为"仅全网"**，把 netease/qq 从配置里摘掉。
    //   理由与默认值同源，见文件头 kDefaultSources 的说明：平台直连源的
    //   "最像原唱的那条下不了"是用户抱怨最多、修不掉的问题。
    //   两个平台源仍注册在 SearchAggregator 里，设置页照常显示、勾了就能用 ——
    //   所以这次迁移是**可逆**的：用户勾回来不会再被迁移掉（版本号已经到 4）。
    //
    //   为什么必须走迁移而不是只改默认值：老用户的 `search/sources` 早就落盘成
    //   `"netease,qq,web"` 了，改 kDefaultSources 对他们是**完全无效**的 ——
    //   那行代码只在"从来没写过这个键"时才生效。
    static const int kSchema = 4;
    const int ver = m_s.value(QStringLiteral("search/schemaVersion"), 1).toInt();
    if (ver >= kSchema) return;

    QStringList list = enabledSources();   // 顺带复用它的"带引号/不带引号都认"的容错
    if (list.isEmpty()) {
        // 从来没配过（走的是默认值）：不需要迁移，写个版本号即可
        m_s.setValue(QStringLiteral("search/schemaVersion"), kSchema);
        return;
    }

    QStringList migrated;
    for (const QString& id : list) {
        // v4：平台源不再默认启用（仍可在设置页手动勾回，勾回后不会再被迁移）
        if (id == QLatin1String("netease") || id == QLatin1String("qq")) {
            qInfo() << "[Settings] 搜索源" << id << "已改为默认关闭（v4：搜索统一走全网）";
            continue;
        }
        // 旧的三条 yt-dlp 源 → 合并成"全网"
        if (id == QLatin1String("bilibili") || id == QLatin1String("ytmusic")
            || id == QLatin1String("soundcloud")) {
            if (!migrated.contains(QStringLiteral("web"))) migrated << QStringLiteral("web");
            continue;
        }
        if (id == QLatin1String("web")) {
            if (!migrated.contains(id)) migrated << id;
            continue;
        }
        // 其余（jamendo/fma/archive/pixabay）没有替代品，直接丢弃
        qInfo() << "[Settings] 搜索源" << id << "已下线，迁移时移除";
    }
    // 摘掉平台源后一个都不剩（或者本来只勾了平台源）→ 补上"全网"，
    // 否则他会看到"没有启用任何搜索源"而不知道怎么办。
    if (migrated.isEmpty()) migrated << QStringLiteral("web");

    m_s.setValue(QStringLiteral("search/sources"), migrated.join(QLatin1Char(',')));
    m_s.setValue(QStringLiteral("search/schemaVersion"), kSchema);
    qInfo() << "[Settings] 搜索源配置已升级到 v" << kSchema << ":" << migrated;
}

void Settings::setEnabledSources(const QStringList& ids)
{
    m_s.setValue(QStringLiteral("search/sources"), ids.join(QLatin1Char(',')));
}

// ---------------------------------------------------------------------------
bool Settings::autoConvertForDevice() const
{
    return m_s.value(QStringLiteral("device/autoConvert"), true).toBool();
}

int Settings::deviceBitrate() const
{
    return qBound(96, m_s.value(QStringLiteral("device/bitrate"), 320).toInt(), 320);
}

// ---------------------------------------------------------------------------
QString Settings::jamendoClientId() const
{
    // 可用环境变量覆盖，方便 CI / 临时调试
    const QString fromEnv = qEnvironmentVariable("MELODYSYNC_JAMENDO_CLIENT_ID");
    if (!fromEnv.isEmpty()) return fromEnv;
    return m_s.value(QStringLiteral("search/jamendoClientId")).toString();
}

QString Settings::pixabayKey() const
{
    const QString fromEnv = qEnvironmentVariable("MELODYSYNC_PIXABAY_KEY");
    if (!fromEnv.isEmpty()) return fromEnv;
    return m_s.value(QStringLiteral("search/pixabayKey")).toString();
}

// ---------------------------------------------------------------------------
//  平台登录态
// ---------------------------------------------------------------------------
QString Settings::neteaseCookie() const
{
    return m_s.value(QStringLiteral("platform/neteaseCookie")).toString();
}

void Settings::setNeteaseCookie(const QString& cookie)
{
    // 每次扫码都会换一组 cookie，直接覆盖；空串表示登出
    m_s.setValue(QStringLiteral("platform/neteaseCookie"), cookie);
    m_s.sync();
}

QString Settings::qqCookie() const
{
    return m_s.value(QStringLiteral("platform/qqCookie")).toString();
}

void Settings::setQQCookie(const QString& cookie)
{
    m_s.setValue(QStringLiteral("platform/qqCookie"), cookie);
    m_s.sync();
}

// ---------------------------------------------------------------------------
QString Settings::ytDlpPath() const
{
    // 同样允许环境变量覆盖：便携版可以在启动脚本里 set 一下，不用碰 ini。
    const QString fromEnv = qEnvironmentVariable("MELODYSYNC_YTDLP_PATH");
    if (!fromEnv.isEmpty()) return fromEnv;
    return m_s.value(QStringLiteral("tools/ytDlpPath")).toString();
}

// ---------------------------------------------------------------------------
QJsonObject Settings::all() const
{
    QJsonObject o;
    o[QStringLiteral("downloadDir")]  = downloadDir();
    o[QStringLiteral("concurrent")]   = concurrent();
    o[QStringLiteral("retries")]      = retries();
    o[QStringLiteral("preferQuality")] = preferQuality();

    o[QStringLiteral("metadata")]     = writeMetadata();
    o[QStringLiteral("cover")]        = embedCover();
    o[QStringLiteral("lyrics")]       = fetchLyrics();

    o[QStringLiteral("aiProvider")]   = aiProvider();
    o[QStringLiteral("aiBaseUrl")]    = aiBaseUrl();
    o[QStringLiteral("aiModel")]      = aiModel();
    o[QStringLiteral("aiKeySet")]     = !aiApiKey().isEmpty();   // 只回"是否设置过"，不回传密钥
    o[QStringLiteral("aiThinking")]   = aiThinkingVisible();

    o[QStringLiteral("cloudEnabled")]  = cloudEnabled();

    o[QStringLiteral("theme")]         = theme();
    o[QStringLiteral("enabledSources")] = QJsonArray::fromStringList(enabledSources());

    o[QStringLiteral("autoConvert")]   = autoConvertForDevice();
    o[QStringLiteral("bitrate")]       = deviceBitrate();

    o[QStringLiteral("jamendoClientId")] = jamendoClientId();
    o[QStringLiteral("pixabayKeySet")]   = !pixabayKey().isEmpty();
    o[QStringLiteral("ytDlpPath")]       = ytDlpPath();
    return o;
}

bool Settings::save(const QJsonObject& p)
{
    auto str  = [&p](const char* k) { return p.value(QLatin1String(k)).toString(); };
    auto num  = [&p](const char* k, int def) {
        const QJsonValue v = p.value(QLatin1String(k));
        return v.isDouble() ? v.toInt() : def;
    };
    auto flag = [&p](const char* k, bool def) {
        const QJsonValue v = p.value(QLatin1String(k));
        return v.isBool() ? v.toBool() : def;
    };

    if (p.contains(QStringLiteral("downloadDir")))  setDownloadDir(str("downloadDir"));
    if (p.contains(QStringLiteral("concurrent")))   setConcurrent(num("concurrent", 3));
    if (p.contains(QStringLiteral("retries")))      setRetries(num("retries", 2));
    if (p.contains(QStringLiteral("preferQuality")))
        m_s.setValue(QStringLiteral("download/quality"), str("preferQuality"));

    if (p.contains(QStringLiteral("metadata")))
        m_s.setValue(QStringLiteral("meta/write"), flag("metadata", true));
    if (p.contains(QStringLiteral("cover")))
        m_s.setValue(QStringLiteral("meta/cover"), flag("cover", true));
    if (p.contains(QStringLiteral("lyrics")))
        m_s.setValue(QStringLiteral("meta/lyrics"), flag("lyrics", false));

    if (p.contains(QStringLiteral("aiProvider")))
        m_s.setValue(QStringLiteral("ai/provider"), str("aiProvider"));
    if (p.contains(QStringLiteral("aiBaseUrl")))
        m_s.setValue(QStringLiteral("ai/baseUrl"), str("aiBaseUrl"));
    if (p.contains(QStringLiteral("aiModel")))
        m_s.setValue(QStringLiteral("ai/model"), str("aiModel"));
    if (p.contains(QStringLiteral("aiApiKey"))) {
        const QString k = str("aiApiKey");
        if (!k.isEmpty()) m_s.setValue(QStringLiteral("ai/apiKey"), k);  // 空字符串表示"不修改"
    }
    if (p.contains(QStringLiteral("aiThinking")))
        m_s.setValue(QStringLiteral("ai/thinking"), flag("aiThinking", true));

    if (p.contains(QStringLiteral("cloudEnabled")))
        m_s.setValue(QStringLiteral("cloud/enabled"), flag("cloudEnabled", true));
    if (p.contains(QStringLiteral("leanCloudAppId")))
        m_s.setValue(QStringLiteral("cloud/lcAppId"), str("leanCloudAppId"));
    if (p.contains(QStringLiteral("leanCloudAppKey")))
        m_s.setValue(QStringLiteral("cloud/lcAppKey"), str("leanCloudAppKey"));
    if (p.contains(QStringLiteral("leanCloudApiBase")))
        m_s.setValue(QStringLiteral("cloud/lcApiBase"), str("leanCloudApiBase"));

    if (p.contains(QStringLiteral("theme")))
        setTheme(str("theme"));
    if (p.contains(QStringLiteral("enabledSources"))) {
        const QJsonArray arr = p.value(QStringLiteral("enabledSources")).toArray();
        QStringList ids;
        for (const QJsonValue& v : arr) ids << v.toString();
        if (!ids.isEmpty()) setEnabledSources(ids);
    }

    if (p.contains(QStringLiteral("autoConvert")))
        m_s.setValue(QStringLiteral("device/autoConvert"), flag("autoConvert", true));
    if (p.contains(QStringLiteral("bitrate")))
        m_s.setValue(QStringLiteral("device/bitrate"), num("bitrate", 320));

    if (p.contains(QStringLiteral("jamendoClientId")))
        m_s.setValue(QStringLiteral("search/jamendoClientId"), str("jamendoClientId"));
    if (p.contains(QStringLiteral("pixabayKey"))) {
        const QString k = str("pixabayKey");
        if (!k.isEmpty()) m_s.setValue(QStringLiteral("search/pixabayKey"), k);
    }

    // ytDlpPath 与 aiApiKey 不同：这里允许被显式清空，
    // 因为"清空"是一个有意义的操作（恢复成自动查找）。
    if (p.contains(QStringLiteral("ytDlpPath")))
        m_s.setValue(QStringLiteral("tools/ytDlpPath"), str("ytDlpPath"));

    m_s.sync();
    return true;
}

} // namespace ms
