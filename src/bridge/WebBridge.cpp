// ============================================================================
//  src/bridge/WebBridge.cpp
// ============================================================================
#include "bridge/WebBridge.h"

#include "ai/AiClient.h"
#include "ai/IntentParser.h"
#include "audio/FFmpegTranscoder.h"
#include "auth/CloudSync.h"
#include "auth/UserManager.h"
#include "bridge/LocalAudioServer.h"
#include "core/Async.h"
#include "core/HttpClient.h"
#include "core/Types.h"
#include "data/Database.h"
#include "device/DeviceManager.h"
#include "device/MtpSync.h"
#include "device/UsbSync.h"
#include "download/DownloadEngine.h"
#include "download/YtDlpWrapper.h"
#include "playlist/PlatformResolve.h"
#include "playlist/PlaylistParser.h"
#include "search/SearchAggregator.h"
#include "settings/Settings.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTextStream>
#include <QUrl>
#include <QUuid>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace ms {

// 同步任务的状态（跨线程共享）
struct WebBridge::SyncState
{
    CancelTokenPtr cancel = std::make_shared<CancelToken>();
    bool running = false;
    QString deviceId;
};

namespace {

QString toCompact(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString toCompact(const QJsonArray& a)
{
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

QJsonObject err(const QString& msg)
{
    QJsonObject o;
    o[QStringLiteral("ok")] = false;
    o[QStringLiteral("error")] = msg;
    return o;
}

// ---------------------------------------------------------------------------
// 桥接层的统一契约：前端取结果靠「调方法 + 等信号」，**只看信号，不看返回值**
// （QWebChannel 的 Q_INVOKABLE 返回值在 JS 侧同步拿不到，详见 README §5）。
//
// 所以任何提前返回都必须把同样的 JSON 通过对应信号发出去。漏发会怎样：
// 前端会一直阻塞到超时才回落 —— 用户感受到的是「点了没反应，十几秒后才弹错」，
// 而且超时兜底还会让调用方误判成「后端不可用」而去灌演示数据。
//
// 用法：return MS_EARLY_ERR(authResult, "后端未初始化");
//
// 注意这里必须是**表达式**（lambda 立即调用），不能用 do { } while (0)：
// 展开点是 `if (...) return MS_EARLY_ERR(...);`，而 `return do{...}while(0);`
// 本身就不是合法语句（C2059: 语法错误"do"）。
//
// 参数名用 SIG 而不是 SIGNAL —— Qt 自带一个 SIGNAL(a) 宏（旧式 connect 用），
// 重名虽然不会真的出错（宏参数替换优先），但读代码时极易看错。
// ---------------------------------------------------------------------------
#define MS_EARLY_ERR(SIG, MSG)                                                 \
    [&]() -> QString {                                                         \
        const QString _msEarlyJson = toCompact(err(QStringLiteral(MSG)));      \
        emit SIG(_msEarlyJson);                                                \
        return _msEarlyJson;                                                   \
    }()

QJsonObject pending()
{
    QJsonObject o;
    o[QStringLiteral("ok")] = true;
    o[QStringLiteral("pending")] = true;
    return o;
}

// 前端传进来的 songsJson → DownloadTask 列表
QVector<DownloadTask> parseSongs(const QString& songsJson)
{
    QVector<DownloadTask> out;
    const QJsonDocument doc = QJsonDocument::fromJson(songsJson.toUtf8());
    const QJsonArray arr = doc.isArray() ? doc.array()
                                         : doc.object().value(QStringLiteral("songs")).toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        DownloadTask t;
        t.id       = o.value(QStringLiteral("id")).toString();
        t.title    = o.value(QStringLiteral("name")).toString(
                         o.value(QStringLiteral("ttl")).toString());
        t.artist   = o.value(QStringLiteral("artist")).toString(
                         o.value(QStringLiteral("art")).toString());
        t.album    = o.value(QStringLiteral("album")).toString();
        t.sourceId = o.value(QStringLiteral("src")).toString();
        t.quality  = qualityFromKey(o.value(QStringLiteral("q")).toString(
                                        o.value(QStringLiteral("qual")).toString()));
        // 期望时长：换源下载时用来核对候选，见 DownloadTask::durationSec
        t.durationSec = o.value(QStringLiteral("durSec")).toInt();
        t.url      = o.value(QStringLiteral("dl")).toString(
                         o.value(QStringLiteral("url")).toString());
        t.coverUrl = o.value(QStringLiteral("cover")).toString();
        t.searchText = o.value(QStringLiteral("searchText")).toString();
        if (t.searchText.isEmpty())
            t.searchText = (t.artist + QLatin1Char(' ') + t.title).trimmed();

        // 平台曲目 id：前端优先显式给 pid；没给就从 "netease:12345" 这种
        // 全局 id 里切出来。它是下载阶段"重新换直链"的唯一钥匙。
        t.platformId = o.value(QStringLiteral("pid")).toString();
        if (t.platformId.isEmpty() && t.id.contains(QLatin1Char(':')))
            t.platformId = PlatformResolve::platformIdOf(t.id);

        // 全局 id 兼作任务 id 会让"同一首歌换源重下"互相顶掉（去重是按标题+歌手+音质，
        // 不按 id），但界面上需要一个稳定可见的键，保持原行为即可。
        if (t.title.isEmpty()) continue;
        out.append(t);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
WebBridge::WebBridge(QObject* parent) : QObject(parent)
{
    m_sync = std::make_shared<SyncState>();
    m_searchCancel = std::make_shared<CancelToken>();
}

WebBridge::~WebBridge() = default;

void WebBridge::bootstrap()
{
    m_users     = std::make_unique<UserManager>(this);
    m_cloud     = std::make_unique<CloudSync>(this);
    m_search    = std::make_unique<SearchAggregator>(this);
    m_playlists = std::make_unique<PlaylistParser>(this);
    m_downloads = std::make_unique<DownloadEngine>(this);
    m_devices   = std::make_unique<DeviceManager>(this);

    m_users->init();

    // 云端账户凭证（LeanCloud）从设置/编译期常量解析一次
    m_cloud->configureFromSettings();

    // 自动登录
    if (m_users->restoreSession()) {
        m_downloads->setUserId(m_users->currentId());
        // 云端账户恢复心跳（本地 isCloud 用户才需要；普通用户无感）
        if (m_users->currentUser().value(QStringLiteral("isCloud")).toBool())
            m_cloud->resumeSession(m_users->currentName());
        qInfo() << "[Bridge] 自动登录:" << m_users->currentName();
    }

    wireSignals();
    m_devices->start();

    // 后台预热 yt-dlp 健康检查（首次启动可能要几十秒）。
    // 不预热的话，「设置 → 搜索源」在 UI 线程查可用性时会整段卡住。
    YtDlpWrapper::prewarm();

    qInfo() << "[Bridge] 初始化完成，工具已就绪";
}

qint64 WebBridge::currentUserId() const
{
    return m_users ? m_users->currentId() : 0;
}

// ---------------------------------------------------------------------------
void WebBridge::wireSignals()
{
    // 下载引擎 → 前端
    connect(m_downloads.get(), &DownloadEngine::queueUpdated,
            this, &WebBridge::queueUpdated);
    connect(m_downloads.get(), &DownloadEngine::libraryUpdated,
            this, &WebBridge::libraryUpdated);
    connect(m_downloads.get(), &DownloadEngine::progress,
            this, &WebBridge::downloadProgress);
    connect(m_downloads.get(), &DownloadEngine::toast,
            this, &WebBridge::toast);

    // 设备管理器 → 前端
    connect(m_devices.get(), &DeviceManager::devicesChanged,
            this, &WebBridge::devicesUpdated);
    connect(m_devices.get(), &DeviceManager::toast,
            this, &WebBridge::toast);

    // 搜索聚合器 → 前端：逐源进度 + 增量结果
    connect(m_search.get(), &SearchAggregator::sourceProgress,
            this, &WebBridge::aiSearchProgress);
    connect(m_search.get(), &SearchAggregator::sourceResults,
            this, &WebBridge::aiSourceHits);

    // 账户变化 → 前端
    connect(m_users.get(), &UserManager::userChanged,
            this, &WebBridge::userChanged);

    // 云端同步失败时给出轻提示（不阻塞任何操作；心跳失败静默，不烦用户）
    connect(m_cloud.get(), &CloudSync::syncFailed, this,
            [this](const QString& action, const QString& error) {
                if (action == QLatin1String("heartbeat")) return;
                if (action == QLatin1String("playlistPush") ||
                    action == QLatin1String("playlistPull")) {
                    QJsonObject o;
                    o[QStringLiteral("ok")]    = false;
                    o[QStringLiteral("error")] = error;
                    emit cloudPlaylistPulled(toCompact(o));
                    return;
                }
                emit toast(QStringLiteral("云端同步失败，已切换为纯本地模式（%1）").arg(error),
                           QStringLiteral("w"));
            });

    // 云端歌单拉取结果 → 前端
    connect(m_cloud.get(), &CloudSync::playlistPulled, this,
            [this](const QString& itemsJson) {
                QJsonObject o;
                o[QStringLiteral("ok")]       = true;
                o[QStringLiteral("hasCloud")] = !itemsJson.isEmpty();
                if (!itemsJson.isEmpty())
                    o[QStringLiteral("items")] = itemsJson;
                emit cloudPlaylistPulled(toCompact(o));
            });

    // 云端歌单推送成功 → 前端回执（与失败分支共用 cloudPlaylistPulled 通道）
    connect(m_cloud.get(), &CloudSync::syncSucceeded, this,
            [this](const QString& action, const QString&) {
                if (action != QLatin1String("playlistPush")) return;
                QJsonObject o;
                o[QStringLiteral("ok")]    = true;
                o[QStringLiteral("hasCloud")] = false;
                emit cloudPlaylistPulled(toCompact(o));
            });

    // 跨设备登录：云端验证通过 → 镜像建户（本地库无同名才建）→ 正式登录
    connect(m_cloud.get(), &CloudSync::cloudLoginResult, this,
            [this](bool ok, const QString& name, const QString& pwdHash,
                   const QString& avatar, const QString& error) {
                if (!ok) {
                    QJsonObject o;
                    o[QStringLiteral("ok")]    = false;
                    o[QStringLiteral("error")] =
                        QStringLiteral("账户不存在或密码错误（云端验证: %1）").arg(error);
                    emit authResult(toCompact(o));
                    return;
                }

                // 本地没有同名账户 → 把云端账户镜像下来（isCloud=true）
                QJsonObject res = m_users->registerUser(name, pwdHash, avatar, true);
                if (!res.value(QStringLiteral("ok")).toBool()) {
                    // 同名本地账户已存在（可能密码不同）→ 试一次本地登录
                    res = m_users->loginUser(name, pwdHash);
                    if (!res.value(QStringLiteral("ok")).toBool()) {
                        QJsonObject o;
                        o[QStringLiteral("ok")]    = false;
                        o[QStringLiteral("error")] =
                            QStringLiteral("本地已有同名账户且密码不一致，请用本地密码登录");
                        emit authResult(toCompact(o));
                        return;
                    }
                }

                m_downloads->setUserId(m_users->currentId());
                // 会话在 cloudLogin 成功时已缓存，这里恢复心跳
                m_cloud->resumeSession(name);
                emit toast(QStringLiteral("欢迎回来，%1（已从云端同步账户）").arg(name), QString());
                emit authResult(toCompact(res));
            });
}

void WebBridge::notifyFrontend(const QString& event, const QString& payload)
{
    QJsonObject o;
    o[QStringLiteral("event")]   = event;
    o[QStringLiteral("payload")] = payload;
    emit toast(toCompact(o), QStringLiteral("event"));
}

// ===========================================================================
//  账户
// ===========================================================================
QString WebBridge::registerUser(const QString& name, const QString& pwdHash,
                                const QString& avatar, bool isCloud)
{
    if (!m_users) return MS_EARLY_ERR(authResult, "后端未初始化");

    const QJsonObject res = m_users->registerUser(name, pwdHash, avatar, isCloud);
    if (res.value(QStringLiteral("ok")).toBool()) {
        m_downloads->setUserId(m_users->currentId());
        if (isCloud) {
            // 勾选了才上报；失败会被 CloudSync 降级成本地模式
            m_cloud->reportRegister(name, pwdHash, avatar);
        }
        emit toast(QStringLiteral("账户创建成功，欢迎 %1").arg(name), QString());
    }
    emit authResult(toCompact(res));
    return toCompact(res);
}

QString WebBridge::loginUser(const QString& name, const QString& pwdHash)
{
    if (!m_users) return MS_EARLY_ERR(authResult, "后端未初始化");

    const QJsonObject res = m_users->loginUser(name, pwdHash);
    if (res.value(QStringLiteral("ok")).toBool()) {
        m_downloads->setUserId(m_users->currentId());
        // 只有云端账户才换取云端会话并开始心跳
        const QJsonObject user = res.value(QStringLiteral("user")).toObject();
        if (user.value(QStringLiteral("isCloud")).toBool()) {
            m_cloud->reportLogin(name, pwdHash);
        }
        emit authResult(toCompact(res));
        return toCompact(res);
    }

    // 本地查无此账户（或密码不符）→ 云端验证回落（跨设备登录）。
    // LeanCloud 校验通过后，cloudLoginResult 里把云端账户镜像成本地账户
    // 再补发 authResult；失败也补发（任何路径都保证前端等得到回包）。
    if (m_cloud->isConfigured()) {
        m_cloud->cloudLogin(name, pwdHash);
        QJsonObject pending;
        pending[QStringLiteral("ok")]      = false;
        pending[QStringLiteral("pending")] = true;
        return toCompact(pending);
    }

    emit authResult(toCompact(res));
    return toCompact(res);
}

void WebBridge::logoutUser()
{
    if (!m_users) return;
    m_users->logoutUser();
    m_downloads->setUserId(0);
    m_cloud->clearSession();   // 停云端心跳并清会话缓存
    emit toast(QStringLiteral("已退出登录"), QString());
}

QString WebBridge::getCurrentUser()
{
    if (!m_users) return QStringLiteral("{}");
    const QString json = toCompact(m_users->currentUser());
    emit userChanged(json);
    return json;
}

QString WebBridge::updateUser(const QString& patchJson)
{
    if (!m_users) return MS_EARLY_ERR(authResult, "后端未初始化");
    const QJsonObject patch = QJsonDocument::fromJson(patchJson.toUtf8()).object();
    const QString json = toCompact(m_users->updateUser(patch));
    emit authResult(json);
    return json;
}

QString WebBridge::deleteUser()
{
    if (!m_users) return MS_EARLY_ERR(authResult, "后端未初始化");

    const qint64 id = m_users->currentId();
    if (id <= 0) return MS_EARLY_ERR(authResult, "未登录");

    // 先停掉属于该用户的下载任务，避免删完账户还有线程在写库
    m_downloads->clearAll();
    m_downloads->setUserId(0);

    const QJsonObject res = m_users->deleteUser();
    if (res.value(QStringLiteral("ok")).toBool())
        emit libraryUpdated(QStringLiteral("[]"));
    emit authResult(toCompact(res));
    return toCompact(res);
}

// ===========================================================================
//  歌单
// ===========================================================================
QString WebBridge::parsePlaylist(const QString& url, const QString& platform)
{
    // 空链接也算"解析结果"的一种，必须走信号 —— 否则前端白等到 40 秒超时
    if (url.trimmed().isEmpty()) return MS_EARLY_ERR(playlistParsed, "请输入歌单链接");

    const QString hint = platform.trimmed();

    runAsync([this, url, hint]() {
        ParsedPlaylist pl;
        QString error;
        bool ok = false;

        if (m_playlists) ok = m_playlists->parse(url, hint, &pl, &error);
        else error = QStringLiteral("解析器未初始化");

        QJsonObject result;
        if (ok) {
            result = pl.toJson();
            result[QStringLiteral("loggedIn")] = m_playlists && m_playlists->neteaseLoggedIn();
        } else {
            result = err(error);
        }
        const QString json = toCompact(result);
        runOnMain([this, json]() { emit playlistParsed(json); });
    });

    return toCompact(pending());
}

QString WebBridge::qrLoginCreate()
{
    runAsync([this]() {
        QString error;
        QJsonObject o;
        if (m_playlists) o = m_playlists->qrCreate(&error);
        else             o = err(QStringLiteral("解析器未初始化"));
        o[QStringLiteral("action")] = QStringLiteral("qr_create");
        if (!error.isEmpty()) o[QStringLiteral("error")] = error;
        const QString json = toCompact(o);
        // 走独立信号，避免与 parsePlaylist 的 playlistParsed 互相干扰
        runOnMain([this, json]() { emit qrLoginResult(json); });
    });
    return toCompact(pending());
}

QString WebBridge::qrLoginPoll(const QString& unikey)
{
    runAsync([this, unikey]() {
        QString error;
        QJsonObject o;
        if (m_playlists) o = m_playlists->qrPoll(unikey, &error);
        else             o = err(QStringLiteral("解析器未初始化"));
        o[QStringLiteral("action")] = QStringLiteral("qr_poll");
        if (!error.isEmpty()) o[QStringLiteral("error")] = error;
        const QString json = toCompact(o);
        runOnMain([this, json]() { emit qrLoginResult(json); });
    });
    return toCompact(pending());
}

QString WebBridge::getPlaylists()
{
    const QString json = toCompact(Database::instance().playlists(currentUserId()));
    emit playlistsLoaded(json);
    return json;
}

QString WebBridge::getPlaylistDetail(int playlistId)
{
    // 与 getPlaylists 一致：同步返回的同时补发一次信号，
    // 让前端可以用统一的「等信号」模式解包，不必依赖 QWebChannel 的返回值回调。
    const QString json = toCompact(Database::instance().playlistDetail(playlistId));
    emit playlistDetailLoaded(json);
    return json;
}

QString WebBridge::deletePlaylist(int playlistId)
{
    const qint64 uid = currentUserId();
    if (uid <= 0) {
        // 前端在这里等的是 playlistsLoaded（靠它刷新列表），所以早退也得把
        // 清单发一份回去，否则列表会僵住 8 秒才超时。
        emit playlistsLoaded(QStringLiteral("[]"));
        return toCompact(err(QStringLiteral("请先登录")));
    }

    QJsonObject res;
    if (!Database::instance().removePlaylist(uid, playlistId)) {
        res = err(QStringLiteral("删除歌单失败"));
    } else {
        res[QStringLiteral("ok")] = true;
        emit toast(QStringLiteral("歌单已删除"), QString());
    }
    // 前端收到 playlistsLoaded 后会自动重绘列表
    emit playlistsLoaded(toCompact(Database::instance().playlists(uid)));
    return toCompact(res);
}

QString WebBridge::savePlaylist(const QString& playlistJson)
{
    const qint64 uid = currentUserId();
    if (uid <= 0) {
        const QString j = toCompact(err(QStringLiteral("请先登录")));
        emit playlistSaved(j);
        return j;
    }

    const QJsonObject o = QJsonDocument::fromJson(playlistJson.toUtf8()).object();
    ParsedPlaylist pl;
    pl.platform  = o.value(QStringLiteral("platform")).toString();
    pl.name      = o.value(QStringLiteral("name")).toString();
    pl.sourceUrl = o.value(QStringLiteral("sourceUrl")).toString();
    pl.creator   = o.value(QStringLiteral("creator")).toString();
    pl.playCount = o.value(QStringLiteral("playCount")).toString();
    const QJsonArray songs = o.value(QStringLiteral("songs")).toArray();
    for (const QJsonValue& v : songs) pl.songs.append(PlaylistTrack::fromJson(v.toObject()));

    if (pl.name.isEmpty()) pl.name = QStringLiteral("未命名歌单");

    const qint64 pid = PlaylistParser::saveToLibrary(uid, pl);
    if (pid <= 0) {
        const QString j = toCompact(err(QStringLiteral("保存歌单失败")));
        emit playlistSaved(j);
        return j;
    }

    QJsonObject res;
    res[QStringLiteral("ok")]   = true;
    res[QStringLiteral("id")]   = pid;
    res[QStringLiteral("name")] = pl.name;
    res[QStringLiteral("count")] = pl.songs.size();
    emit toast(QStringLiteral("已保存歌单《%1》（%2 首）").arg(pl.name).arg(pl.songs.size()),
               QString());
    // 云端账户：整包推送 LeanCloud（last-write-wins，失败静默降级）
    const QJsonObject cu = m_users->currentUser();
    if (cu.value(QStringLiteral("isCloud")).toBool())
        m_cloud->pushPlaylist(cu.value(QStringLiteral("name")).toString(), playlistJson);
    // 通知前端：保存结果 + 刷新歌单列表
    emit playlistSaved(toCompact(res));
    emit playlistsLoaded(toCompact(Database::instance().playlists(uid)));
    return toCompact(res);
}

// ---------------------------------------------------------------------------
// 云端歌单手动同步（个人中心按钮）
// ---------------------------------------------------------------------------
void WebBridge::cloudPushPlaylist(const QString& itemsJson)
{
    if (!m_users || !m_users->loggedIn()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] = QStringLiteral("请先登录");
        emit cloudPlaylistPulled(toCompact(o));
        return;
    }
    const QJsonObject cu = m_users->currentUser();
    if (!cu.value(QStringLiteral("isCloud")).toBool()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] = QStringLiteral("云端歌单同步仅支持云端账户");
        emit cloudPlaylistPulled(toCompact(o));
        return;
    }
    m_cloud->pushPlaylist(cu.value(QStringLiteral("name")).toString(), itemsJson);
}

void WebBridge::cloudPullPlaylist()
{
    if (!m_users || !m_users->loggedIn()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] = QStringLiteral("请先登录");
        emit cloudPlaylistPulled(toCompact(o));
        return;
    }
    const QJsonObject cu = m_users->currentUser();
    if (!cu.value(QStringLiteral("isCloud")).toBool()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] = QStringLiteral("云端歌单同步仅支持云端账户");
        emit cloudPlaylistPulled(toCompact(o));
        return;
    }
    m_cloud->pullPlaylist(cu.value(QStringLiteral("name")).toString());
}

// ===========================================================================
//  AI 全网搜
// ===========================================================================
QString WebBridge::aiSearch(const QString& query, const QString& sourcesJson)
{
    if (query.trimmed().isEmpty()) return MS_EARLY_ERR(aiSearchDone, "请输入搜索关键词");
    if (!m_search)                 return MS_EARLY_ERR(aiSearchDone, "搜索模块未初始化");

    // 解析启用的源（前端传数组；为空则用设置里的默认值）
    QStringList sources;
    const QJsonDocument doc = QJsonDocument::fromJson(sourcesJson.toUtf8());
    if (doc.isArray()) {
        for (const QJsonValue& v : doc.array()) sources << v.toString();
    } else if (doc.isObject()) {
        const QJsonArray arr = doc.object().value(QStringLiteral("sources")).toArray();
        for (const QJsonValue& v : arr) sources << v.toString();
    }
    if (sources.isEmpty()) sources = Settings::instance().enabledSources();

    if (m_searchCancel) m_searchCancel->cancel();
    m_searchCancel = std::make_shared<CancelToken>();

    SearchAggregator::Config cfg;
    cfg.enabledSources  = sources;
    cfg.limitPerSource  = 12;
    cfg.maxParallel     = 8;
    // 搜索总超时：两层执行的兜底上限。
    //   第一层（网易云/QQ）通常 2~3 秒就全回来了，原地结束；
    //   只有第一层给不出"能下载的"结果时才会跑第二层（全网源），
    //   而全网源内部给哔哩哔哩留的预算是"剩余时间的绝大部分"
    //   （PyInstaller 解包 3~5s + 6 条逐条解析约 9s），所以 20s 够用。
    // 到点即取消：yt-dlp 会被杀掉，已输出的条目仍算部分结果。
    cfg.searchTimeoutMs = 20000;
    m_search->setConfig(cfg);

    const CancelTokenPtr cancel = m_searchCancel;

    runAsync([this, query, cancel]() {
        IntentParser parser;
        QString engine, llmError;
        const SearchIntent intent = parser.parse(query, &engine, &llmError);

        SearchAggregator::Result result;
        if (m_search) result = m_search->searchAll(intent, cancel);
        else          result.intent = intent;
        result.engine = engine;

        QJsonObject o = result.toJson();
        if (cancel->isCanceled()) o[QStringLiteral("canceled")] = true;
        // 带出 LLM 失败原因：engine 字段只写得出 "rule"，不足以让用户知道
        // 为什么没走 AI（Key 过期 / 地址错 / 模型名不对，三种表现完全一样）
        if (!llmError.isEmpty()) o[QStringLiteral("engineError")] = llmError;

        const QString json = toCompact(o);
        runOnMain([this, json]() { emit aiSearchDone(json); });
    });

    return toCompact(pending());
}

// ---------------------------------------------------------------------------
// AI 连通性自测
//
// 「AI 搜索用不了」有一大半是配置问题：地址填错、Key 过期、模型名不在服务商
// 的列表里。这三种情况在搜索流程里的表现完全一样 —— 都是静静回落成规则解析，
// 用户只看到「没走 AI」，不知道错在哪。
//
// 这个接口发一次真实的、最小的对话请求，把服务端原话带回来。比任何本地格式
// 校验都可靠：GLM 的 Key 需要 id.secret 两段，光看长度是看不出来的。
// ---------------------------------------------------------------------------
QString WebBridge::testAiConnection()
{
    runAsync([this]() {
        AiClient ai;                 // 构造时 reloadFromSettings，读到的就是最新配置
        QJsonObject o;
        if (!ai.ready()) {
            o = err(QStringLiteral("还没有填 API Key"));
        } else {
            const AiClient::Reply r = ai.chat(
                QStringLiteral("你是连通性测试助手。只回复用户要求的字，不要任何多余内容。"),
                QStringLiteral("回复两个字：可用"),
                0.0, 16);
            if (r.ok) {
                o[QStringLiteral("ok")]       = true;
                o[QStringLiteral("reply")]    = r.text.trimmed();
                o[QStringLiteral("provider")] = ai.providerName();
                o[QStringLiteral("model")]    = ai.model();
            } else {
                o = err(r.error);
            }
        }
        const QString json = toCompact(o);
        runOnMain([this, json]() { emit aiTestResult(json); });
    });
    return toCompact(pending());
}


// 本地音频 → <audio> 可用地址（LocalAudioServer 的 127.0.0.1 端点）。
// 同步返回：只做路径校验和字符串拼接，不碰磁盘 IO 之外的重活。
QString WebBridge::audioUrl(const QString& absolutePath)
{
    return LocalAudioServer::instance().audioUrl(absolutePath);
}

// ===========================================================================
//  歌词
// ===========================================================================
namespace {

// LRC → [{t:秒, s:歌词行}]。兼容多时间标签行（"[00:10.00][02:30.00] 同一句"）
// 和毫秒的 2/3 位写法；跳过纯元数据标签（[ti:][ar:]…）与空行。
QJsonArray parseLrc(const QString& lrcText)
{
    static const QRegularExpression tagRe(
        QStringLiteral("\\[(\\d{1,2}):(\\d{1,2})(?:[.:](\\d{1,3}))?\\]"));

    QVector<QPair<qint64, QString>> rows;
    for (const QString& rawLine : lrcText.split(QLatin1Char('\n'))) {
        QVector<qint64> times;
        auto it = tagRe.globalMatch(rawLine);
        qsizetype lastEnd = -1;
        while (it.hasNext()) {
            const auto m = it.next();
            const qint64 fracMs = m.captured(3).isEmpty()
                ? 0
                : (m.captured(3).length() == 2
                       ? m.captured(3).toInt() * 10      // 厘秒
                       : m.captured(3).toInt());          // 毫秒（<1000，换算后乘 1）
            const qint64 t = m.captured(1).toInt() * 60 + m.captured(2).toInt()
                             + fracMs / 1000;
            times.append(t);
            lastEnd = m.capturedEnd();
        }
        if (times.isEmpty()) continue;
        const QString text = rawLine.mid(static_cast<int>(lastEnd)).trimmed();
        if (text.isEmpty()) continue;      // 间奏空行：跳过，不占高亮行
        for (qint64 t : times)
            rows.append({ t, text });
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    QJsonArray arr;
    for (const auto& r : rows) {
        QJsonObject l;
        l[QStringLiteral("t")] = r.first;
        l[QStringLiteral("s")] = r.second;
        arr.append(l);
    }
    return arr;
}

// 网易云歌词接口需要的公共头
QStringList neteaseHeaders()
{
    return { QStringLiteral("Referer: https://music.163.com/"),
             QStringLiteral("Cookie: appver=2.0.2") };
}

} // namespace

void WebBridge::fetchLyrics(const QString& title, const QString& artist,
                            const QString& localPath)
{
    // Q_INVOKABLE 跑在 UI 线程，HTTP 必须丢进后台线程；
    // 跨线程只用值拷贝（this 的信号是线程安全的 queued 投递）。
    runAsync([this, title, artist, localPath]() {
        QJsonObject res;
        res[QStringLiteral("ok")]     = false;
        res[QStringLiteral("source")] = QString();
        res[QStringLiteral("lines")]  = QJsonArray();
        res[QStringLiteral("title")]  = title;

        // ---- 1) 本地同名 .lrc（下载器顺手生成的场景）----
        if (!localPath.isEmpty() && QFileInfo::exists(localPath)) {
            const QFileInfo fi(localPath);
            const QString lrcPath =
                fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".lrc"));
            QFile f(lrcPath);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QJsonArray lines =
                    parseLrc(QString::fromUtf8(f.readAll()));
                if (!lines.isEmpty()) {
                    res[QStringLiteral("ok")]     = true;
                    res[QStringLiteral("source")] = QStringLiteral("local");
                    res[QStringLiteral("lines")]  = lines;
                    emit lyricsReady(toCompact(res));
                    return;
                }
            }
        }

        // ---- 2) 网易云：搜索匹配 songId → lyric API 拿 LRC ----
        if (!title.isEmpty()) {
            const QString kw = (title + QLatin1Char(' ') + artist).trimmed();

            QList<QPair<QString, QString>> form;
            form << qMakePair(QStringLiteral("s"), kw)
                 << qMakePair(QStringLiteral("type"), QStringLiteral("1"))
                 << qMakePair(QStringLiteral("offset"), QStringLiteral("0"))
                 << qMakePair(QStringLiteral("limit"), QStringLiteral("5"))
                 << qMakePair(QStringLiteral("total"), QStringLiteral("true"));
            const HttpClient::Response sr = HttpClient::postForm(
                QStringLiteral("https://music.163.com/api/search/get/web"),
                form, neteaseHeaders(), 10000);

            if (sr.ok) {
                const QJsonArray songs = sr.json().value(QStringLiteral("result"))
                                             .toObject().value(QStringLiteral("songs"))
                                             .toArray();

                // 挑选：歌手名对得上 > 标题更接近 > 取第一条
                int best = -1, bestScore = -1;
                for (int i = 0; i < songs.size(); ++i) {
                    const QJsonObject s = songs.at(i).toObject();
                    int score = 1;
                    if (!artist.isEmpty()
                        && s.value(QStringLiteral("artists")).toArray()
                               .at(0).toObject().value(QStringLiteral("name"))
                               .toString().contains(artist, Qt::CaseInsensitive))
                        score += 5;
                    if (s.value(QStringLiteral("name")).toString()
                            .compare(title, Qt::CaseInsensitive) == 0)
                        score += 3;
                    if (score > bestScore) { bestScore = score; best = i; }
                }

                if (best >= 0) {
                    const QJsonObject s = songs.at(best).toObject();
                    const QString songId =
                        QString::number(s.value(QStringLiteral("id"))
                                            .toVariant().toLongLong());
                    const QString songName = s.value(QStringLiteral("name")).toString();
                    const QString songArtist =
                        s.value(QStringLiteral("artists")).toArray()
                             .at(0).toObject().value(QStringLiteral("name")).toString();

                    const HttpClient::Response lr = HttpClient::getJson(
                        QStringLiteral("https://music.163.com/api/song/lyric?id=%1"
                                       "&lv=1&kv=1&tv=-1").arg(songId),
                        neteaseHeaders(), 10000);
                    if (lr.ok) {
                        const QString lrc = lr.json().value(QStringLiteral("lrc"))
                                                .toObject()
                                                .value(QStringLiteral("lyric"))
                                                .toString();
                        const QJsonArray lines = parseLrc(lrc);
                        if (!lines.isEmpty()) {
                            res[QStringLiteral("ok")]      = true;
                            res[QStringLiteral("source")]  = QStringLiteral("netease");
                            res[QStringLiteral("lines")]   = lines;
                            res[QStringLiteral("songId")]  = songId;
                            res[QStringLiteral("matched")] =
                                songArtist + QStringLiteral(" - ") + songName;
                        }
                    }
                }
            }
        }

        if (!res.value(QStringLiteral("ok")).toBool())
            res[QStringLiteral("message")] = QStringLiteral("没找到可用歌词");

        emit lyricsReady(toCompact(res));
    });
}

// ===========================================================================
//  音乐库：从磁盘删除（回收站）+ 目录校验
// ===========================================================================
QString WebBridge::deleteLibraryFile(int itemId)
{
    if (currentUserId() <= 0) return toCompact(err(QStringLiteral("未登录")));

    const QVector<LibraryItem> lib = Database::instance().library(currentUserId());
    QString path, name;
    for (const LibraryItem& it : lib) {
        if (it.id == itemId) { path = it.filePath; name = it.title; break; }
    }
    if (path.isEmpty() || !QFileInfo::exists(path))
        return toCompact(err(QStringLiteral("文件不存在或已被移动：%1").arg(path)));

#ifdef Q_OS_WIN
    // 移入回收站而不是直接抹掉 —— 手滑删错还能从回收站捞回来。
    // SHFileOperation 的 pFrom 要求双 NUL 结尾，std::wstring 再补一个刚好。
    const std::wstring from =
        QDir::toNativeSeparators(path).toStdWString() + std::wstring(1, L'\0');
    SHFILEOPSTRUCTW op{};
    op.hwnd   = nullptr;
    op.wFunc  = FO_DELETE;
    op.pFrom  = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (SHFileOperationW(&op) != 0)
        return toCompact(err(QStringLiteral("删除失败（文件可能被占用）")));
#else
    if (!QFile::remove(path))
        return toCompact(err(QStringLiteral("删除失败（文件可能被占用）")));
#endif

    // 顺手清掉配套的 .lrc（是本应用生成的，留着就是垃圾文件）
    const QFileInfo fi(path);
    const QString lrc = fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".lrc"));
    if (QFileInfo::exists(lrc)) QFile::remove(lrc);

    Database::instance().removeLibraryItem(currentUserId(), itemId);
    emit libraryUpdated(getLibrary());
    emit toast(QStringLiteral("已删除《%1》的文件（移入回收站，可在回收站恢复）").arg(name),
               QString());

    QJsonObject o;
    o[QStringLiteral("ok")]     = true;
    o[QStringLiteral("deleted")] = path;
    return toCompact(o);
}

QString WebBridge::ensureDir(const QString& path)
{
    const QString clean = QDir::fromNativeSeparators(path.trimmed());
    if (clean.isEmpty())
        return toCompact(err(QStringLiteral("路径不能为空")));
    // 只接受绝对路径：相对路径会在 exe 工作目录里悄悄建出一串深目录
    if (!QDir::isAbsolutePath(clean))
        return toCompact(err(QStringLiteral("请填写完整绝对路径（如 C:\\Music\\MelodySync）")));
    if (!QDir().mkpath(clean))
        return toCompact(err(QStringLiteral("无法创建目录（权限不足？）")));
    QTemporaryFile probe(clean + QStringLiteral("/.ms_probe"));
    if (!probe.open())
        return toCompact(err(QStringLiteral("目录不可写（被占用或没有写权限）")));
    probe.close();
    probe.remove();

    QJsonObject o;
    o[QStringLiteral("ok")]  = true;
    o[QStringLiteral("dir")] = QDir::toNativeSeparators(clean);
    return toCompact(o);
}

QString WebBridge::getSources()
{
    QJsonArray arr;
    const SearchAggregator::Config cfg = m_search ? m_search->config()
                                                   : SearchAggregator::Config();
    const QStringList enabled = Settings::instance().enabledSources();

    if (m_search) {
        // 可用性单独查一次：缺 sidecar（yt-dlp）的源、网络不通的站点，
        // 都得在设置页里直接说明原因，否则用户只能对着一排勾选框猜。
        QHash<QString, QPair<bool, QString>> status;   // id → (可用, 原因)
        for (const SearchAggregator::SourceStatus& st : m_search->sourceStatus())
            status.insert(st.id, qMakePair(st.available, st.reason));

        const QHash<QString, SourceMeta> metas = m_search->allMetas();
        for (auto it = metas.constBegin(); it != metas.constEnd(); ++it) {
            const SourceMeta& m = it.value();
            QJsonObject o;
            o[QStringLiteral("id")]      = m.id;
            o[QStringLiteral("name")]    = m.name;
            o[QStringLiteral("icon")]    = m.icon;
            o[QStringLiteral("desc")]    = m.desc;
            o[QStringLiteral("free")]    = m.free;
            o[QStringLiteral("trust")]   = m.trust;
            // 0 = 平台直连（先跑）/ 1 = 全网兜底（第一层不够时才跑）。
            // 设置页要按这个分组展示，用户才看得懂"两层检索"是怎么回事。
            o[QStringLiteral("tier")]    = m.tier;
            o[QStringLiteral("license")] = m.license;
            o[QStringLiteral("enabled")] = enabled.contains(m.id);
            const auto s = status.constFind(m.id);
            if (s != status.constEnd()) {
                o[QStringLiteral("available")] = s.value().first;
                o[QStringLiteral("reason")]    = s.value().second;
            }
            arr.append(o);
        }
    }
    Q_UNUSED(cfg)
    const QString json = toCompact(arr);
    emit sourcesLoaded(json);
    return json;
}

void WebBridge::cancelSearch()
{
    if (m_searchCancel) m_searchCancel->cancel();
    emit toast(QStringLiteral("已取消本次搜索"), QStringLiteral("w"));
}

// ===========================================================================
//  下载
// ===========================================================================
void WebBridge::addToQueue(const QString& songsJson)
{
    if (!m_downloads) return;
    const QVector<DownloadTask> tasks = parseSongs(songsJson);
    if (tasks.isEmpty()) {
        emit toast(QStringLiteral("没有可下载的歌曲（付费源仅支持试听）"), QStringLiteral("w"));
        return;
    }
    m_downloads->enqueue(tasks);
}

void WebBridge::startAll()
{
    if (m_downloads) m_downloads->startAll();
}

void WebBridge::pauseAll()
{
    if (m_downloads) m_downloads->pauseAll();
}

void WebBridge::clearDone()
{
    if (m_downloads) m_downloads->clearDone();
}

void WebBridge::cleanQueue()
{
    if (m_downloads) m_downloads->clearAll();
}

void WebBridge::retryFailed()
{
    if (m_downloads) m_downloads->retryFailed();
}

void WebBridge::cancelTask(const QString& taskId)
{
    if (m_downloads) m_downloads->cancelTask(taskId);
}

void WebBridge::removeTask(const QString& taskId)
{
    if (m_downloads) m_downloads->removeTask(taskId);
}

QString WebBridge::getQueue()
{
    if (!m_downloads) return QStringLiteral("[]");
    const QString json = m_downloads->queueJson();
    emit queueUpdated(json);
    return json;
}

// ===========================================================================
//  音乐库
// ===========================================================================
QString WebBridge::getLibrary()
{
    const QVector<LibraryItem> lib = Database::instance().library(currentUserId());
    QJsonArray arr;
    for (const LibraryItem& it : lib) arr.append(it.toJson());
    const QString json = toCompact(arr);
    emit libraryUpdated(json);
    return json;
}

QString WebBridge::getAnalysis()
{
    const QString json = toCompact(Database::instance().libraryStats(currentUserId()));
    emit analysisLoaded(json);
    return json;
}

QString WebBridge::getHistory()
{
    const QString json = toCompact(Database::instance().history(currentUserId(), 300));
    emit historyLoaded(json);
    return json;
}

void WebBridge::clearHistory()
{
    const qint64 uid = currentUserId();
    if (uid <= 0) return;
    Database::instance().clearHistory(uid);
    emit historyLoaded(QStringLiteral("[]"));
    emit toast(QStringLiteral("下载历史已清空"), QString());
}

void WebBridge::removeLibraryItem(int itemId)
{
    if (currentUserId() <= 0) return;
    // 只从库里移除记录，不删磁盘文件（删文件必须由用户显式操作）
    Database::instance().removeLibraryItem(currentUserId(), itemId);
    emit libraryUpdated(getLibrary());
    emit toast(QStringLiteral("已从音乐库移除（文件仍保留在磁盘上）"), QString());
}

void WebBridge::clearLibrary()
{
    if (currentUserId() <= 0) return;
    Database::instance().clearLibrary(currentUserId());
    emit libraryUpdated(QStringLiteral("[]"));
    emit analysisLoaded(toCompact(Database::instance().libraryStats(currentUserId())));
    emit toast(QStringLiteral("音乐库已清空（磁盘文件未被删除）"), QString());
}

// ===========================================================================
//  设备
// ===========================================================================
QString WebBridge::getDevices()
{
    if (!m_devices) return QStringLiteral("[]");
    const QString json = m_devices->devicesJson();
    emit devicesUpdated(json);
    return json;
}

void WebBridge::refreshDevices()
{
    if (m_devices) m_devices->refresh();
}

void WebBridge::selectDevice(const QString& deviceId)
{
    if (!m_devices) return;
    const DeviceInfo d = m_devices->deviceById(deviceId);
    if (d.id.isEmpty()) {
        emit toast(QStringLiteral("设备已断开"), QStringLiteral("w"));
        return;
    }
    emit toast(QStringLiteral("已选择设备：%1").arg(d.name), QString());
}

// 生成同步计划（异步）：只读扫描，不写任何东西，让用户先确认再同步
QString WebBridge::planSync(const QString& deviceId)
{
    if (!m_devices) return MS_EARLY_ERR(syncDone, "设备模块未初始化");

    const DeviceInfo dev = m_devices->deviceById(deviceId);
    if (dev.id.isEmpty()) return MS_EARLY_ERR(syncDone, "设备不存在");

    const Settings& cfg = Settings::instance();
    const QString srcDir = cfg.downloadDir();
    const CancelTokenPtr cancel = m_sync ? m_sync->cancel : CancelTokenPtr();
    if (cancel) cancel->reset();

    runAsync([this, dev, srcDir, cancel]() {
        UsbSync::Options opt;
        opt.targetSubdir  = QStringLiteral("Music");
        opt.incremental   = true;
        opt.convert       = dev.mtp ? false : !FFmpegTranscoder::deviceSupports(dev, QStringLiteral("flac"));
        opt.targetBitrate = Settings::instance().deviceBitrate();

        QJsonObject result;
        if (dev.mtp) {
            // MTP 设备拿不完整文件表，计划里只列本地待传清单
            const QStringList files = UsbSync::collectAudioFiles(srcDir, cancel);
            QStringList existing = MtpSync::listRemoteFiles(dev.id, opt.targetSubdir, nullptr);
            QJsonArray arr;
            int skip = 0;
            for (const QString& f : files) {
                const QFileInfo fi(f);
                const bool has = existing.contains(fi.fileName(), Qt::CaseInsensitive);
                if (has) ++skip;
                QJsonObject o;
                o[QStringLiteral("name")] = fi.fileName();
                o[QStringLiteral("size")] = fi.size();
                o[QStringLiteral("skip")] = has;
                o[QStringLiteral("reason")] = has ? QStringLiteral("设备上已存在同名文件")
                                                  : QStringLiteral("待传输");
                arr.append(o);
            }
            result[QStringLiteral("total")] = files.size();
            result[QStringLiteral("skip")]  = skip;
            result[QStringLiteral("copy")]  = files.size() - skip;
            result[QStringLiteral("convert")] = 0;
            result[QStringLiteral("items")] = arr;
        } else {
            const UsbSync::Plan plan = UsbSync::buildPlan(srcDir, dev, opt, cancel);
            result = QJsonDocument::fromJson(plan.toJson().toUtf8()).object();
        }

        result[QStringLiteral("ok")]     = true;
        result[QStringLiteral("action")] = QStringLiteral("plan");
        result[QStringLiteral("device")] = dev.name;

        const QString json = toCompact(result);
        runOnMain([this, json]() { emit syncDone(json); });
    });

    return toCompact(pending());
}

QString WebBridge::startSync(const QString& deviceId, const QString& scope)
{
    if (!m_devices) return MS_EARLY_ERR(syncDone, "设备模块未初始化");
    if (!m_sync) return MS_EARLY_ERR(syncDone, "同步状态未初始化");
    if (m_sync->running) return MS_EARLY_ERR(syncDone, "已有同步任务在进行中");

    const DeviceInfo dev = m_devices->deviceById(deviceId);
    if (dev.id.isEmpty()) return MS_EARLY_ERR(syncDone, "设备不存在或已拔出");

    const QString srcDir = Settings::instance().downloadDir();
    const CancelTokenPtr cancel = m_sync->cancel;
    cancel->reset();
    m_sync->running = true;
    m_sync->deviceId = deviceId;

    runAsync([this, dev, srcDir, scope, cancel]() {
        const Settings& cfg = Settings::instance();

        UsbSync::Options opt;
        opt.targetSubdir  = QStringLiteral("Music");
        opt.incremental   = true;
        opt.convert       = !dev.mtp
                            && !FFmpegTranscoder::deviceSupports(dev, QStringLiteral("flac"));
        opt.targetBitrate = cfg.deviceBitrate();
        opt.verifyAfterCopy = true;

        // 进度回调：节流后推到前端（否则每拷贝一个文件都刷一次 UI 会抖）
        Throttle throttle(150);
        auto onProgress = [this, &throttle, cancel](int percent, int cur, int total,
                                                    const QString& name) {
            if (!throttle.ready()) return;
            runOnMain([this, percent, cur, total]() {
                emit syncProgress(percent, cur, total);
            });
            Q_UNUSED(name)
            Q_UNUSED(cancel)
        };

        QString error;
        bool ok = false;

        if (dev.mtp) {
            const QStringList files = UsbSync::collectAudioFiles(srcDir, cancel);
            MtpSync::Options mo;
            mo.targetFolder = opt.targetSubdir;
            mo.existingNames = MtpSync::listRemoteFiles(dev.id, mo.targetFolder, nullptr);
            ok = MtpSync::pushFiles(dev.id, files, mo,
                                    [&](int pct, int cur, int total, const QString& name) {
                                        onProgress(pct, cur, total, name);
                                    },
                                    cancel, &error);
        } else {
            const UsbSync::Plan plan = UsbSync::buildPlan(srcDir, dev, opt, cancel);
            UsbSync usb;
            ok = usb.execute(plan, opt, onProgress, cancel, &error);
            if (ok) {
                // 统计信息放到结果里，前端可以展示"跳过了多少首"
                QJsonObject extra;
                extra[QStringLiteral("skip")]    = plan.skipCount;
                extra[QStringLiteral("copy")]    = plan.copyCount;
                extra[QStringLiteral("convert")] = plan.convertCount;
                error.clear();
                Q_UNUSED(extra)
            }
        }

        QJsonObject result;
        result[QStringLiteral("ok")]       = ok;
        result[QStringLiteral("action")]   = QStringLiteral("sync");
        result[QStringLiteral("device")]   = dev.name;
        result[QStringLiteral("canceled")] = cancel->isCanceled();
        if (!ok) result[QStringLiteral("error")] = error;

        const QString json = toCompact(result);
        runOnMain([this, json, ok]() {
            m_sync->running = false;
            emit syncDone(json);
            if (ok) emit toast(QStringLiteral("同步完成，可以安全拔出设备"), QString());
        });
    });

    return toCompact(pending());
}

void WebBridge::cancelSync()
{
    if (m_sync && m_sync->cancel) {
        m_sync->cancel->cancel();
        emit toast(QStringLiteral("正在停止同步…"), QStringLiteral("w"));
    }
}

// ===========================================================================
//  设置
// ===========================================================================
QString WebBridge::getSettings()
{
    const QString json = toCompact(Settings::instance().all());
    emit settingsLoaded(json);
    return json;
}

QString WebBridge::saveSettings(const QString& json)
{
    const QJsonObject patch = QJsonDocument::fromJson(json.toUtf8()).object();
    Settings& cfg = Settings::instance();
    cfg.save(patch);

    // 立即生效的项：并发数、云端凭证
    if (patch.contains(QStringLiteral("concurrent")) && m_downloads)
        m_downloads->setConcurrency(cfg.concurrent());
    if ((patch.contains(QStringLiteral("cloudEnabled")) ||
         patch.contains(QStringLiteral("cloudApiBase"))) && m_cloud)
        m_cloud->configureFromSettings();
    if (patch.contains(QStringLiteral("enabledSources")) && m_search) {
        SearchAggregator::Config c = m_search->config();
        c.enabledSources = cfg.enabledSources();
        m_search->setConfig(c);
    }

    QJsonObject res;
    res[QStringLiteral("ok")]       = true;
    res[QStringLiteral("settings")] = cfg.all();
    emit settingsLoaded(toCompact(cfg.all()));
    return toCompact(res);
}

// ===========================================================================
//  播放 / 杂项
// ===========================================================================
void WebBridge::playDemo(const QString& songId)
{
    // 说明：试听播放由前端的 <audio> 元素承担（拿 streamUrl 直接播），
    //       这里只做"记录 + 广播"，方便将来接入原生播放器。
    qInfo() << "[Bridge] playDemo:" << songId;
    emit toast(QStringLiteral("正在试听：%1").arg(songId), QString());
}

void WebBridge::togglePlay()
{
    emit toast(QStringLiteral("播放/暂停"), QString());
}

QString WebBridge::openInExplorer(const QString& path)
{
    const QFileInfo fi(path);
    const QString target = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    if (!QFileInfo::exists(target)) return toCompact(err(QStringLiteral("路径不存在")));

#ifdef Q_OS_WIN
    // explorer 选中文件的写法：explorer /select,"路径"
    const QStringList args = fi.isDir()
        ? QStringList{ QDir::toNativeSeparators(target) }
        : QStringList{ QStringLiteral("/select,") + QDir::toNativeSeparators(fi.absoluteFilePath()) };
    QProcess::startDetached(QStringLiteral("explorer.exe"), args);
#else
    QProcess::startDetached(QStringLiteral("xdg-open"), { target });
#endif
    QJsonObject o;
    o[QStringLiteral("ok")] = true;
    return toCompact(o);
}

QString WebBridge::appInfo()
{
    QJsonObject o;
    o[QStringLiteral("name")]    = QStringLiteral("MelodySync");
    o[QStringLiteral("version")] = QStringLiteral("1.0.0");
    o[QStringLiteral("qt")]      = QStringLiteral(QT_VERSION_STR);
    o[QStringLiteral("dataDir")] = Database::instance().path();
    o[QStringLiteral("downloadDir")] = Settings::instance().downloadDir();
    o[QStringLiteral("disclaimer")] =
        QStringLiteral("本软件仅限个人学习与研究使用，不得用于任何商业目的。"
                       "AI 全网搜仅聚合 CC 协议 / 公版 / 艺术家直供等合法免费源，"
                       "以及各平台的官方试听接口；不提供任何绕过 DRM 或平台会员限制的功能。"
                       "用户应自行承担因下载受版权保护内容而产生的全部法律责任。");
    const QString json = toCompact(o);
    emit appInfoLoaded(json);
    return json;
}

} // namespace ms
