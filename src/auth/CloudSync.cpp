// ============================================================================
//  src/auth/CloudSync.cpp
//  腾讯 CloudBase 云函数传输层。服务端见 tools/cloudbase/ms-account.js：
//    - 单端点 POST <apiBase>，body = {action, username?, token?, ...参数}
//    - 响应 {code:0, ...} 成功 / {code:非0, msg} 失败（401=会话失效 404=无账户）
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
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSettings>
#include <QSysInfo>

namespace ms {

namespace {
const char* kDeviceIdKey  = "cloud/deviceId";
const char* kSessionTokKey = "cloud/apiToken";
const char* kSessionUsrKey = "cloud/apiUsername";

#ifndef MS_APP_VERSION
#  define MS_APP_VERSION "1.0.0"
#endif
// 发布机构建注入（cmake/secrets.cmake，不入库）；没有时回退 Settings 里的值
#ifndef MS_CLOUD_API_BASE
#  define MS_CLOUD_API_BASE ""
#endif
} // namespace

CloudSync::CloudSync(QObject* parent) : QObject(parent)
{
    // 心跳：主线程 QTimer 定时触发，网络动作在 callAction 里投线程池
    m_heartbeat.setInterval(10 * 60 * 1000);
    connect(&m_heartbeat, &QTimer::timeout, this, &CloudSync::heartbeat);
}

// ---------------------------------------------------------------------------
// 端点解析：Settings 用户显式配置 > 编译期注入。都没配 = 纯本地模式。
// ---------------------------------------------------------------------------
void CloudSync::configureFromSettings(const QString& apiBaseOverride)
{
    // 自检探针直连指定端点（绕过总开关与 settings）
    if (!apiBaseOverride.isEmpty()) {
        m_base = apiBaseOverride.trimmed();
        while (m_base.endsWith(QLatin1Char('/'))) m_base.chop(1);
        qInfo() << "[CloudSync] 云端账户自检端点:" << m_base;
        return;
    }

    const Settings& st = Settings::instance();

    if (!st.cloudEnabled()) {
        m_base.clear();
        qInfo() << "[CloudSync] 云端同步总开关已关闭，纯本地模式";
        return;
    }

    m_base = st.cloudApiBase().trimmed();
    while (m_base.endsWith(QLatin1Char('/'))) m_base.chop(1);
    if (m_base.isEmpty()) m_base = QStringLiteral(MS_CLOUD_API_BASE);

    if (!isConfigured())
        qInfo() << "[CloudSync] 未配置云端账户地址，账户云端同步停用（纯本地模式）";
    else
        qInfo() << "[CloudSync] 云端账户就绪:" << m_base;
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
    seed.append(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8());
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
// 统一异步请求：后台线程 POST，结果投回主线程发信号
// ---------------------------------------------------------------------------
void CloudSync::callAction(const QString& action, const QJsonObject& payload, bool withToken)
{
    if (!isConfigured()) return;   // 未配置：静默跳过（纯本地模式）

    QJsonObject body = payload;
    body[QStringLiteral("action")] = action;   // 服务端按 action 分发
    if (withToken && !m_token.isEmpty())
        body[QStringLiteral("token")] = m_token;
    const QByteArray raw = QJsonDocument(body).toJson(QJsonDocument::Compact);
    const QString url = m_base;

    runAsync([this, action, url, raw]() {
        const HttpClient::Response r = HttpClient::postRaw(
            url, raw, QStringLiteral("application/json; charset=utf-8"),
            QStringList(), 12000);

        if (!r.ok) {
            const QString err = r.error.isEmpty() ? QStringLiteral("网络不可用") : r.error;
            qInfo() << "[CloudSync]" << action << "失败（已降级为本地模式）:" << err;
            runOnMain([this, action, err]() { emit syncFailed(action, err); });
            return;
        }

        const QJsonObject resp = r.json();
        if (resp.value(QStringLiteral("code")).toInt() != 0) {
            const QString msg = resp.value(QStringLiteral("msg")).toString();
            runOnMain([this, action, msg]() { emit syncFailed(action, msg); });
            return;
        }

        runOnMain([this, action, resp]() {
            // 注册/登录响应携带 token，顺手缓存（供心跳与歌单同步用）
            if (resp.contains(QLatin1String("token")))
                handleSessionResponse(action, resp);
            emit syncSucceeded(action, QString::fromUtf8(
                QJsonDocument(resp).toJson(QJsonDocument::Compact)));
            // 拉取结果要单独走 playlistPulled（WebBridge 靠它包成
            // cloudPlaylistPulled 回前端；syncSucceeded 只在探针/日志用）
            if (action == QLatin1String("pullPlaylist"))
                emit playlistPulled(resp.value(QStringLiteral("items")).toString());
        });
    });
}

// ---------------------------------------------------------------------------
// 会话缓存：token + 用户名持久化，重启后可恢复心跳
// ---------------------------------------------------------------------------
void CloudSync::handleSessionResponse(const QString& action, const QJsonObject& resp)
{
    const QString tok = resp.value(QStringLiteral("token")).toString();
    const QString usr = resp.value(QStringLiteral("username")).toString();
    if (!tok.isEmpty() && !usr.isEmpty()) {
        m_token     = tok;
        m_username  = usr;
        QSettings s;
        s.setValue(QLatin1String(kSessionTokKey), m_token);
        s.setValue(QLatin1String(kSessionUsrKey), m_username);
        s.sync();
        qInfo() << "[CloudSync]" << action << "成功，云端账户:" << m_username;
    }
    if (!m_username.isEmpty()) startHeartbeat();
}

void CloudSync::startHeartbeat()
{
    if (m_token.isEmpty() || m_username.isEmpty()) return;
    if (!m_heartbeat.isActive()) {
        m_heartbeat.start();
        heartbeat();   // 登录后立即刷一次在线状态，不等第一个周期
    }
}

void CloudSync::resumeSession(const QString& username)
{
    if (!isConfigured()) return;
    QSettings s;
    m_token    = s.value(QLatin1String(kSessionTokKey)).toString();
    m_username = username;
    if (!m_token.isEmpty())
        startHeartbeat();
}

void CloudSync::clearSession()
{
    m_heartbeat.stop();
    m_token.clear();
    m_username.clear();
    QSettings s;
    s.remove(QLatin1String(kSessionTokKey));
    s.remove(QLatin1String(kSessionUsrKey));
    s.sync();
}

// ---------------------------------------------------------------------------
// 注册：本地注册成功后异步建云端账户（passwordHash 为前端弱哈希，非明文）
// ---------------------------------------------------------------------------
void CloudSync::reportRegister(const QString& username, const QString& pwdHash, const QString& avatar)
{
    if (!isConfigured()) return;
    QJsonObject p = baseProfile();
    p[QStringLiteral("username")]     = username;
    p[QStringLiteral("passwordHash")] = pwdHash;
    p[QStringLiteral("avatar")]       = avatar;
    callAction(QStringLiteral("register"), p, false);
}

// ---------------------------------------------------------------------------
// 登录：换 token 并立即刷新 lastActiveAt
// ---------------------------------------------------------------------------
void CloudSync::reportLogin(const QString& username, const QString& pwdHash)
{
    if (!isConfigured()) return;
    QJsonObject p = baseProfile();
    p[QStringLiteral("username")]     = username;
    p[QStringLiteral("passwordHash")] = pwdHash;
    callAction(QStringLiteral("login"), p, false);
}

// ---------------------------------------------------------------------------
// 跨设备登录：到云端验证；成功即缓存会话并回 cloudLoginResult，
// 失败区分「无此账户」（静默，让用户走本地提示）与「密码错误」（明确报错）
// ---------------------------------------------------------------------------
void CloudSync::cloudLogin(const QString& username, const QString& pwdHash)
{
    if (!isConfigured()) return;

    runAsync([this, username, pwdHash]() {
        QJsonObject p = baseProfile();
        p[QStringLiteral("action")]       = QStringLiteral("login");
        p[QStringLiteral("username")]     = username;
        p[QStringLiteral("passwordHash")] = pwdHash;

        const HttpClient::Response r = HttpClient::postRaw(
            m_base, QJsonDocument(p).toJson(QJsonDocument::Compact),
            QStringLiteral("application/json; charset=utf-8"), QStringList(), 12000);

        if (!r.ok) {
            runOnMain([this, username, pwdHash]() {
                emit cloudLoginResult(false, username, pwdHash, QString(), QStringLiteral("网络不可用"));
            });
            return;
        }

        const QJsonObject resp = r.json();
        if (resp.value(QStringLiteral("code")).toInt() != 0) {
            runOnMain([this, username, pwdHash, resp]() {
                emit cloudLoginResult(false, username, pwdHash, QString(),
                                      resp.value(QStringLiteral("msg")).toString());
            });
            return;
        }

        runOnMain([this, resp, username, pwdHash]() {
            handleSessionResponse(QStringLiteral("cloudLogin"), resp);
            emit cloudLoginResult(true, username, pwdHash,
                                  resp.value(QStringLiteral("avatar")).toString(), QString());
        });
    });
}

// ---------------------------------------------------------------------------
// 心跳：刷新 lastActiveAt。会话失效时静默清缓存，等下次登录重建。
// ---------------------------------------------------------------------------
void CloudSync::heartbeat()
{
    if (!isConfigured() || m_token.isEmpty() || m_username.isEmpty()) return;
    QJsonObject p;
    p[QStringLiteral("username")] = m_username;
    callAction(QStringLiteral("heartbeat"), p, true);
}

// ---------------------------------------------------------------------------
// 歌单云同步：整包 last-write-wins（服务端 upsert ms_playlists）
// ---------------------------------------------------------------------------
void CloudSync::pushPlaylist(const QString& username, const QString& itemsJson)
{
    if (!isConfigured()) return;
    QJsonObject p = baseProfile();
    p[QStringLiteral("username")] = username;
    p[QStringLiteral("items")]    = itemsJson;
    callAction(QStringLiteral("pushPlaylist"), p, true);
}

void CloudSync::pullPlaylist(const QString& username)
{
    if (!isConfigured()) return;
    QJsonObject p;
    p[QStringLiteral("username")] = username;
    callAction(QStringLiteral("pullPlaylist"), p, true);
}

} // namespace ms
