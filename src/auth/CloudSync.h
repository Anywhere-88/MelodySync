// ============================================================================
//  src/auth/CloudSync.h
//  可选的云端账户同步。
//
//  设计原则（重要）：
//    - 云端是"锦上添花"，任何失败都必须降级为纯本地模式，不能阻塞登录流程；
//    - 因此所有上报都在后台线程 fire-and-forget，失败只发信号给 UI 提示；
//    - 上传内容不含密码明文，只上传已哈希值 + 设备指纹，用户需显式勾选同意。
// ============================================================================
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace ms {

class CloudSync : public QObject
{
    Q_OBJECT

public:
    explicit CloudSync(QObject* parent = nullptr);

    // workerBase 形如 https://your-worker.workers.dev（不含 /api）
    void setEndpoint(const QString& workerBase);
    QString endpoint() const { return m_base; }

    // 稳定的设备指纹（首次生成后写入 QSettings，长期不变）
    QString deviceId();
    QString osVersion() const;
    QString appVersion() const;

    // 注册上报：POST <base>/api/register
    // body: {username,passwordHash,avatar,deviceId,osVersion,appVersion,registeredAt}
    void reportRegister(const QString& username, const QString& pwdHash, const QString& avatar);

    // 登录上报（更新 last_seen）：POST <base>/api/login
    void reportLogin(const QString& username, const QString& pwdHash);

    // 心跳（可选）：POST <base>/api/heartbeat
    void heartbeat(const QString& username);

signals:
    void syncSucceeded(const QString& action, const QString& responseJson);
    void syncFailed(const QString& action, const QString& error);   // UI 侧降级提示即可

private:
    QJsonObject buildBasePayload(const QString& username, const QString& pwdHash) const;
    void postAsync(const QString& path, const QJsonObject& payload, const QString& action);

    QString m_base;
    QString m_deviceId;
};

} // namespace ms
