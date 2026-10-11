// ============================================================================
//  src/auth/CloudSync.cpp
// ============================================================================
#include "auth/CloudSync.h"

#include "core/Async.h"
#include "core/HttpClient.h"
#include "core/Types.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSettings>
#include <QSysInfo>

namespace ms {

namespace {
const char* kDeviceIdKey = "cloud/deviceId";

#ifndef MS_APP_VERSION
#  define MS_APP_VERSION "1.0.0"
#endif
}

CloudSync::CloudSync(QObject* parent) : QObject(parent)
{
    m_base = QStringLiteral("https://your-worker.workers.dev");
}

void CloudSync::setEndpoint(const QString& workerBase)
{
    QString b = workerBase.trimmed();
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    if (!b.isEmpty()) m_base = b;
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
    seed.append(nowIso().toUtf8());
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

// ---------------------------------------------------------------------------
QJsonObject CloudSync::buildBasePayload(const QString& username, const QString& pwdHash) const
{
    QJsonObject o;
    o[QStringLiteral("username")]     = username;
    o[QStringLiteral("passwordHash")] = pwdHash;      // 已是哈希值，绝非明文
    o[QStringLiteral("deviceId")]     = const_cast<CloudSync*>(this)->deviceId();
    o[QStringLiteral("osVersion")]    = osVersion();
    o[QStringLiteral("appVersion")]   = appVersion();
    return o;
}

void CloudSync::postAsync(const QString& path, const QJsonObject& payload, const QString& action)
{
    const QString url = m_base + path;
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    // 全部放到后台线程：网络抖动绝不能影响用户登录/注册的响应速度
    runAsync([this, url, body, action]() {
        const HttpClient::Response r = HttpClient::postRaw(
            url, body, QStringLiteral("application/json; charset=utf-8"),
            QStringList(), 12000);

        if (r.ok) {
            runOnMain([this, action, r]() {
                emit syncSucceeded(action, r.text());
            });
        } else {
            const QString err = r.error.isEmpty() ? QStringLiteral("网络不可用") : r.error;
            qInfo() << "[CloudSync]" << action << "上报失败（已降级为本地模式）:" << err;
            runOnMain([this, action, err]() {
                emit syncFailed(action, err);
            });
        }
    });
}

void CloudSync::reportRegister(const QString& username, const QString& pwdHash, const QString& avatar)
{
    QJsonObject p = buildBasePayload(username, pwdHash);
    p[QStringLiteral("avatar")]       = avatar;
    p[QStringLiteral("registeredAt")] = nowIso();
    postAsync(QStringLiteral("/api/register"), p, QStringLiteral("register"));
}

void CloudSync::reportLogin(const QString& username, const QString& pwdHash)
{
    QJsonObject p = buildBasePayload(username, pwdHash);
    p[QStringLiteral("lastSeen")] = nowIso();
    postAsync(QStringLiteral("/api/login"), p, QStringLiteral("login"));
}

void CloudSync::heartbeat(const QString& username)
{
    QJsonObject p;
    p[QStringLiteral("username")] = username;
    p[QStringLiteral("deviceId")] = deviceId();
    p[QStringLiteral("ts")]       = nowIso();
    postAsync(QStringLiteral("/api/heartbeat"), p, QStringLiteral("heartbeat"));
}

} // namespace ms
