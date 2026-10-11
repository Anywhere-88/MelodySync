// ============================================================================
//  src/auth/CloudSync.h
//  云端账户同步 —— LeanCloud 国际版 REST（无服务器方案）。
//
//  设计原则（重要）：
//    - 云端是"锦上添花"，任何失败都必须降级为纯本地模式，不能阻塞登录流程；
//    - 因此所有上报都在后台线程 fire-and-forget，失败只发信号给 UI 提示；
//    - 上传内容不含密码明文：LeanCloud _User 的 password 字段存的是前端
//      djb2 弱哈希（与本地 SQLite 的加盐迭代哈希相互独立，见 UserManager）。
//
//  数据模型（LeanCloud 控制台可直接查看）：
//    _User    ：username / password(弱哈希) / avatar / deviceId / osVersion /
//               appVersion / lastActiveAt —— lastActiveAt 半小时内视为在线，
//               开发者在控制台即可看到全部注册用户与在线状态。
//    Playlist ：owner(用户名) / items(歌单 JSON 文本) / deviceId / updatedAt
//               —— 整包 last-write-wins，一个云端账户对应一条歌单记录。
//
//  凭证来源（优先级）：Settings(用户可覆盖) > 编译期定义(MS_LC_APP_ID 等，
//  发布机构建时由 cmake/secrets.cmake 提供，该文件不入库)。
// ============================================================================
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace ms {

class CloudSync : public QObject
{
    Q_OBJECT

public:
    explicit CloudSync(QObject* parent = nullptr);

    // 从 Settings + 编译期常量解析凭证；缺少 AppId/AppKey 时保持"未配置"，
    // 所有上报静默跳过（纯本地模式）。
    void configureFromSettings();
    bool isConfigured() const { return !m_appId.isEmpty() && !m_appKey.isEmpty(); }

    // 稳定的设备指纹（首次生成后写入 QSettings，长期不变）
    QString deviceId();
    QString osVersion() const;
    QString appVersion() const;

    // 启动自动登录后调用：用缓存的 objectId/会话令牌恢复心跳（仅 isCloud
    // 用户才调）。云端会话失效时首次心跳会静默清除缓存，不影响本地功能。
    void resumeSession(const QString& username);

    // 注册上报：POST /1.1/users 建账（失败降级，不影响本地注册）
    void reportRegister(const QString& username, const QString& pwdHash, const QString& avatar);

    // 登录上报：GET /1.1/login 换取会话令牌，并刷新 lastActiveAt
    void reportLogin(const QString& username, const QString& pwdHash);

    // 跨设备登录：本地库查无此账户时，到 LeanCloud 验证用户名+弱哈希。
    // 成功后发 cloudLoginResult(ok=true)，由 WebBridge 把云端账户镜像成本地账户。
    void cloudLogin(const QString& username, const QString& pwdHash);

    // 心跳：PUT /1.1/users/<objectId> 刷新 lastActiveAt（10 分钟一次）
    void heartbeat();

    // 歌单云同步（整包 last-write-wins）。itemsJson 为歌单内容 JSON 文本。
    void pushPlaylist(const QString& username, const QString& itemsJson);
    void pullPlaylist(const QString& username);

    // 退出登录：清掉云端会话缓存并停止心跳
    void clearSession();

signals:
    void syncSucceeded(const QString& action, const QString& responseJson);
    void syncFailed(const QString& action, const QString& error);   // UI 侧降级提示即可
    void playlistPulled(const QString& itemsJson);                  // 空串 = 云端还没有歌单

    // 跨设备登录回执：ok 时 userJson 带 username/avatar（云端档案），
    // 失败时 error 说明原因（含"云端无此账户/密码不符"）
    void cloudLoginResult(bool ok, const QString& username, const QString& pwdHash,
                          const QString& avatar, const QString& error);

private:
    void startHeartbeat();
    void requestAsync(const QString& method, const QString& path,
                      const QByteArray& body, const QString& action,
                      bool withSession);
    void handleSessionResponse(const QString& action, const QJsonObject& resp);
    QStringList lcHeaders(bool withSession) const;
    QJsonObject baseProfile() const;

    QString  m_appId;
    QString  m_appKey;
    QString  m_base;              // REST 域名，形如 https://xxxxxxxx.api.lncldglobal.com
    QString  m_deviceId;
    QString  m_sessionToken;      // LeanCloud 会话令牌（登录/注册后缓存）
    QString  m_objectId;          // _User 的 objectId
    QString  m_username;          // 当前云端账户名
    QTimer   m_heartbeat;         // 主线程定时器，网络动作仍投到线程池
};

} // namespace ms
