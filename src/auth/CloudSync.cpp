// ============================================================================
//  src/auth/CloudSync.cpp
//  LeanCloud 国际版 REST 传输层。接口文档要点：
//    - 请求头：X-LC-Id / X-LC-Key（/ X-LC-Session 登录态）
//    - 注册：POST /1.1/users  {username, password, ...自定义字段}
//    - 登录：GET  /1.1/login?username=&password=  → 返回 objectId+sessionToken
//    - 更新：PUT  /1.1/users/<objectId>（登录态）
//    - 自建类：/1.1/classes/Playlist
//    - Date 字段统一用 {"__type":"Date","iso":"..."} 编码
//  所有网络动作经 runAsync 投到线程池，主线程只收信号。
// ============================================================================
#include "auth/CloudSync.h"

#include "core/Async.h"
#include "core/HttpClient.h"
#include "core/Types.h"
#include "settings/Settings.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSettings>
#include <QSysInfo>
#include <QUrlQuery>

namespace ms {

namespace {
const char* kDeviceIdKey   = "cloud/deviceId";
const char* kSessionObjKey = "cloud/lcObjectId";
const char* kSessionTokKey = "cloud/lcSessionToken";
const char* kSessionUsrKey = "cloud/lcUsername";

#ifndef MS_APP_VERSION
#  define MS_APP_VERSION "1.0.0"
#endif
// 发布机构建注入（cmake/secrets.cmake，不入库）；没有时回退 Settings 里的值
#ifndef MS_LC_APP_ID
#  define MS_LC_APP_ID ""
#endif
#ifndef MS_LC_APP_KEY
#  define MS_LC_APP_KEY ""
#endif
#ifndef MS_LC_API_BASE
#  define MS_LC_API_BASE ""
#endif

// LeanCloud Date 字段编码
QJsonObject lcDate(const QDateTime& dt)
{
    QJsonObject o;
    o[QStringLiteral("__type")] = QStringLiteral("Date");
    o[QStringLiteral("iso")]    = dt.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'"));
    return o;
}

QString lcNowIso() { return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'")); }
} // namespace

CloudSync::CloudSync(QObject* parent) : QObject(parent)
{
    // 心跳：主线程 QTimer 定时触发，网络动作在 requestAsync 里投线程池
    m_heartbeat.setInterval(10 * 60 * 1000);
    connect(&m_heartbeat, &QTimer::timeout, this, &CloudSync::heartbeat);
}

// ---------------------------------------------------------------------------
// 凭证解析：Settings 用户显式配置 > 编译期注入。都没配 = 纯本地模式。
// ---------------------------------------------------------------------------
void CloudSync::configureFromSettings()
{
    const Settings& st = Settings::instance();

    if (!st.cloudEnabled()) {
        m_appId.clear(); m_appKey.clear(); m_base.clear();
        qInfo() << "[CloudSync] 云端同步总开关已关闭，纯本地模式";
        return;
    }

    m_appId  = st.leanCloudAppId();
    m_appKey = st.leanCloudAppKey();
    m_base   = st.leanCloudApiBase().trimmed();
    while (m_base.endsWith(QLatin1Char('/'))) m_base.chop(1);

    if (m_appId.isEmpty())  m_appId  = QStringLiteral(MS_LC_APP_ID);
    if (m_appKey.isEmpty()) m_appKey = QStringLiteral(MS_LC_APP_KEY);
    if (m_base.isEmpty())   m_base   = QStringLiteral(MS_LC_API_BASE);

    // 兜底：给了 AppId 但没给域名时，按国际版规则推导（AppId 前 8 位小写）
    if (!m_appId.isEmpty() && m_base.isEmpty())
        m_base = QStringLiteral("https://%1.api.lncldglobal.com")
                     .arg(m_appId.left(8).toLower());

    if (!isConfigured())
        qInfo() << "[CloudSync] 未配置 LeanCloud 凭证，账户云端同步停用（纯本地模式）";
    else
        qInfo() << "[CloudSync] LeanCloud 就绪:" << m_base;
}

QStringList CloudSync::lcHeaders(bool withSession) const
{
    QStringList h;
    h << QStringLiteral("X-LC-Id: %1").arg(m_appId);
    h << QStringLiteral("X-LC-Key: %1").arg(m_appKey);
    h << QStringLiteral("Content-Type: application/json; charset=utf-8");
    if (withSession && !m_sessionToken.isEmpty())
        h << QStringLiteral("X-LC-Session: %1").arg(m_sessionToken);
    return h;
}

// ---------------------------------------------------------------------------
// 设备指纹：随机 16 字节 + 机器标识派生，首次生成后固化到 QSettings。
// 不采集任何硬件序列号，只做"同一台机器稳定、不同机器不同"的最小标识。
// ---------------------------------------------------------------------------
QString CloudSync::deviceId()
{
    if (!m_deviceId.isEmpty()) return m_deviceId;

    QSettings s;
    m_deviceId = s.value(QLatin1String(kDeviceIdKey)).toString();
    if (!m_deviceId.isEmpty()) return m_deviceId;

    QByteArray seed = QSysInfo::machineUniqueId();
    if (seed.isEmpty()) {
        seed.resize(16);
        QRandomGenerator::system()->generate(seed.begin(), seed.end());
    }
    seed.append(lcNowIso().toUtf8());
    m_deviceId = QString::fromLatin1(
        QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex().left(32));

    s.setValue(QLatin1String(kDeviceIdKey), m_deviceId);
    s.sync();
    return m_deviceId;
}

QString CloudSync::osVersion() const
{
    return QStringLiteral("%1 %2 (build %3)")
        .arg(QSysInfo::productType() == QLatin1String("windows") ? QStringLiteral("Windows")
                                                                 : QSysInfo::prettyProductName(),
             QSysInfo::productVersion(),
             QSysInfo::kernelVersion());
}

QString CloudSync::appVersion() const { return QStringLiteral(MS_APP_VERSION); }

QJsonObject CloudSync::baseProfile() const
{
    QJsonObject o;
    o[QStringLiteral("deviceId")]   = const_cast<CloudSync*>(this)->deviceId();
    o[QStringLiteral("osVersion")]  = osVersion();
    o[QStringLiteral("appVersion")] = appVersion();
    return o;
}

// ---------------------------------------------------------------------------
// 统一异步请求：后台线程发请求，结果投回主线程发信号
// ---------------------------------------------------------------------------
void CloudSync::requestAsync(const QString& method, const QString& path,
                             const QByteArray& body, const QString& action,
                             bool withSession)
{
    if (!isConfigured()) return;   // 未配置：静默跳过（纯本地模式）

    const QString url  = m_base + path;
    const QStringList headers = lcHeaders(withSession);

    runAsync([this, method, url, body, headers, action, withSession]() {
        const HttpClient::Response r = HttpClient::request(method, url, body, headers, 12000);

        if (!r.ok) {
            const QString err = r.error.isEmpty() ? QStringLiteral("网络不可用") : r.error;
            qInfo() << "[CloudSync]" << action << "失败（已降级为本地模式）:" << err;
            runOnMain([this, action, err]() { emit syncFailed(action, err); });
            return;
        }

        const QJsonObject resp = r.json();
        // LeanCloud 业务错误：HTTP 4xx/5xx 已算 !ok，但个别 2xx 带错误体的
        // 情况也兜一层（code 字段存在即视为失败）
        if (resp.contains(QLatin1String("code")) && !resp.contains(QLatin1String("objectId"))) {
            const QString err = resp.value(QStringLiteral("error")).toString();
            runOnMain([this, action, err]() { emit syncFailed(action, err); });
            return;
        }

        runOnMain([this, action, resp, withSession]() {
            // 注册/登录响应携带会话信息，顺手缓存（供心跳与歌单同步用）
            if (withSession || resp.contains(QLatin1String("sessionToken")))
                handleSessionResponse(action, resp);
            emit syncSucceeded(action, QString::fromUtf8(
                QJsonDocument(resp).toJson(QJsonDocument::Compact)));
        });
    });
}

// ---------------------------------------------------------------------------
// 会话缓存：objectId + sessionToken + 用户名持久化，重启后可恢复心跳
// ---------------------------------------------------------------------------
void CloudSync::handleSessionResponse(const QString& action, const QJsonObject& resp)
{
    const QString oid = resp.value(QStringLiteral("objectId")).toString();
    const QString tok = resp.value(QStringLiteral("sessionToken")).toString();
    if (!oid.isEmpty() && !tok.isEmpty()) {
        m_objectId     = oid;
        m_sessionToken = tok;
        m_username     = resp.value(QStringLiteral("username")).toString();
        QSettings s;
        s.setValue(QLatin1String(kSessionObjKey), m_objectId);
        s.setValue(QLatin1String(kSessionTokKey), m_sessionToken);
        s.setValue(QLatin1String(kSessionUsrKey), m_username);
        s.sync();
        qInfo() << "[CloudSync]" << action << "成功，云端账户:" << m_username;
    }
    if (!m_username.isEmpty()) startHeartbeat();
}

void CloudSync::startHeartbeat()
{
    if (m_objectId.isEmpty() || m_sessionToken.isEmpty()) return;
    if (!m_heartbeat.isActive()) {
        m_heartbeat.start();
        heartbeat();   // 登录后立即刷一次在线状态，不等第一个周期
    }
}

void CloudSync::resumeSession(const QString& username)
{
    if (!isConfigured()) return;
    QSettings s;
    m_objectId     = s.value(QLatin1String(kSessionObjKey)).toString();
    m_sessionToken = s.value(QLatin1String(kSessionTokKey)).toString();
    m_username     = username;
    if (!m_objectId.isEmpty() && !m_sessionToken.isEmpty())
        startHeartbeat();
}

void CloudSync::clearSession()
{
    m_heartbeat.stop();
    m_objectId.clear();
    m_sessionToken.clear();
    m_username.clear();
    QSettings s;
    s.remove(QLatin1String(kSessionObjKey));
    s.remove(QLatin1String(kSessionTokKey));
    s.remove(QLatin1String(kSessionUsrKey));
    s.sync();
}

// ---------------------------------------------------------------------------
// 注册：本地注册成功后异步建云端 _User（password 存前端弱哈希，非明文）
// ---------------------------------------------------------------------------
void CloudSync::reportRegister(const QString& username, const QString& pwdHash, const QString& avatar)
{
    if (!isConfigured()) return;
    QJsonObject p = baseProfile();
    p[QStringLiteral("username")]     = username;
    p[QStringLiteral("password")]     = pwdHash;
    p[QStringLiteral("avatar")]       = avatar;
    p[QStringLiteral("lastActiveAt")] = lcDate(QDateTime::currentDateTimeUtc());
    requestAsync(QStringLiteral("POST"), QStringLiteral("/1.1/users"),
                 QJsonDocument(p).toJson(QJsonDocument::Compact),
                 QStringLiteral("register"), false);
}

// ---------------------------------------------------------------------------
// 登录：换会话令牌并立即刷新 lastActiveAt
// ---------------------------------------------------------------------------
void CloudSync::reportLogin(const QString& username, const QString& pwdHash)
{
    if (!isConfigured()) return;
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("username"), HttpClient::urlEncode(username));
    q.addQueryItem(QStringLiteral("password"), HttpClient::urlEncode(pwdHash));
    requestAsync(QStringLiteral("GET"),
                 QStringLiteral("/1.1/login?") + q.toString(QUrl::FullyEncoded),
                 QByteArray(), QStringLiteral("login"), false);
}

// ---------------------------------------------------------------------------
// 心跳：刷新 lastActiveAt。会话失效（403/404）时静默清缓存，等下次登录重建。
// ---------------------------------------------------------------------------
void CloudSync::heartbeat()
{
    if (!isConfigured() || m_objectId.isEmpty() || m_sessionToken.isEmpty()) return;
    QJsonObject p;
    p[QStringLiteral("lastActiveAt")] = lcDate(QDateTime::currentDateTimeUtc());
    requestAsync(QStringLiteral("PUT"),
                 QStringLiteral("/1.1/users/") + m_objectId,
                 QJsonDocument(p).toJson(QJsonDocument::Compact),
                 QStringLiteral("heartbeat"), true);
}

// ---------------------------------------------------------------------------
// 歌单云同步：GET 查 owner 现有记录 → 有则 PUT 整包覆盖，无则 POST 新建
// ---------------------------------------------------------------------------
void CloudSync::pushPlaylist(const QString& username, const QString& itemsJson)
{
    if (!isConfigured()) return;

    const QString owner = username;
    const QString items = itemsJson;
    const QString action = QStringLiteral("playlistPush");

    runAsync([this, owner, items, action]() {
        // 1) 查现有记录
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("where"),
                       HttpClient::urlEncode(QStringLiteral("{\"owner\":\"%1\"}").arg(owner)));
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
        const HttpClient::Response qr = HttpClient::request(
            QStringLiteral("GET"), m_base + QStringLiteral("/1.1/classes/Playlist?") +
                                       q.toString(QUrl::FullyEncoded),
            QByteArray(), lcHeaders(false), 12000);

        const QJsonArray results = qr.json().value(QStringLiteral("results")).toArray();
        const QString existingId = results.isEmpty()
                                       ? QString()
                                       : results.first().toObject().value(QStringLiteral("objectId")).toString();

        // 2) 整包覆盖或新建
        QJsonObject payload = baseProfile();
        payload[QStringLiteral("owner")]     = owner;
        payload[QStringLiteral("items")]     = items;
        payload[QStringLiteral("updatedAt")] = lcDate(QDateTime::currentDateTimeUtc());

        const QString method = existingId.isEmpty() ? QStringLiteral("POST") : QStringLiteral("PUT");
        const QString path   = existingId.isEmpty()
                                   ? QStringLiteral("/1.1/classes/Playlist")
                                   : QStringLiteral("/1.1/classes/Playlist/") + existingId;

        const HttpClient::Response wr = HttpClient::request(
            method, m_base + path,
            QJsonDocument(payload).toJson(QJsonDocument::Compact),
            lcHeaders(false), 15000);

        if (!wr.ok) {
            const QString err = wr.error.isEmpty() ? QStringLiteral("网络不可用") : wr.error;
            qInfo() << "[CloudSync]" << action << "失败:" << err;
            runOnMain([this, action, err]() { emit syncFailed(action, err); });
            return;
        }
        qInfo() << "[CloudSync]" << action << "成功，字节数:" << items.size();
        runOnMain([this, action]() {
            emit syncSucceeded(action, QStringLiteral("{}"));
        });
    });
}

void CloudSync::pullPlaylist(const QString& username)
{
    if (!isConfigured()) return;

    const QString owner  = username;
    const QString action = QStringLiteral("playlistPull");

    runAsync([this, owner, action]() {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("where"),
                       HttpClient::urlEncode(QStringLiteral("{\"owner\":\"%1\"}").arg(owner)));
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
        const HttpClient::Response r = HttpClient::request(
            QStringLiteral("GET"), m_base + QStringLiteral("/1.1/classes/Playlist?") +
                                       q.toString(QUrl::FullyEncoded),
            QByteArray(), lcHeaders(false), 12000);

        if (!r.ok) {
            const QString err = r.error.isEmpty() ? QStringLiteral("网络不可用") : r.error;
            qInfo() << "[CloudSync]" << action << "失败:" << err;
            runOnMain([this, action, err]() { emit syncFailed(action, err); });
            return;
        }

        const QJsonArray results = r.json().value(QStringLiteral("results")).toArray();
        const QString items = results.isEmpty() ? QString()
            : results.first().toObject().value(QStringLiteral("items")).toString();

        runOnMain([this, items]() { emit playlistPulled(items); });
    });
}

// ---------------------------------------------------------------------------
// 跨设备登录：到 LeanCloud 验证；成功即缓存会话并回 cloudLoginResult，
// 失败区分「无此账户」（静默，让用户走本地提示）与「密码不符」（明确报错）
// ---------------------------------------------------------------------------
void CloudSync::cloudLogin(const QString& username, const QString& pwdHash)
{
    if (!isConfigured()) return;

    const QString user = username;
    const QString hash = pwdHash;

    runAsync([this, user, hash]() {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("username"), HttpClient::urlEncode(user));
        q.addQueryItem(QStringLiteral("password"), HttpClient::urlEncode(hash));
        const HttpClient::Response r = HttpClient::request(
            QStringLiteral("GET"),
            m_base + QStringLiteral("/1.1/login?") + q.toString(QUrl::FullyEncoded),
            QByteArray(), lcHeaders(false), 12000);

        if (!r.ok) {
            // 401/404 = 云端没有这个账户（或密码不符）。区分文案：
            const QJsonObject errObj = r.json();
            const int code = errObj.value(QStringLiteral("code")).toInt();
            const bool noUser = (r.status == 404) || code == 201 || code == 210 || code == 211;
            // 210=用户名/密码错误 211=找不到用户
            const QString msg = noUser ? QStringLiteral("云端无此账户")
                                       : (errObj.value(QStringLiteral("error")).toString());
            runOnMain([this, user, hash, noUser, msg]() {
                emit cloudLoginResult(false, user, hash, QString(), msg);
            });
            return;
        }

        const QJsonObject resp = r.json();
        // 顺手缓存会话（供心跳用）：cloudLoginResult 处理完镜像建户后，
        // WebBridge 会调 resumeSession()，而这里先写 QSettings 保证幂等
        runOnMain([this, resp, user, hash]() {
            handleSessionResponse(QStringLiteral("cloudLogin"), resp);
            emit cloudLoginResult(true, user, hash,
                                  resp.value(QStringLiteral("avatar")).toString(), QString());
        });
    });
}

} // namespace ms
