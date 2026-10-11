// ============================================================================
//  src/auth/CloudSync.h
//  云端账户同步 —— 腾讯云 CloudBase 云函数 REST（无服务器方案）。
//
//  设计原则（重要）：
//    - 云端是"锦上添花"，任何失败都必须降级为纯本地模式，不能阻塞登录流程；
//    - 因此所有上报都在后台线程 fire-and-forget，失败只发信号给 UI 提示；
//    - 上传内容不含密码明文：云端 passwordHash 存的是前端 djb2 弱哈希，
//      服务端再叠加随机盐 scrypt 落库（与本地 UserManager 的哈希相互独立）。
//
//  为什么选 CloudBase：LeanCloud 已公告 2027-01-12 彻底关停（2026-01-12 起
//  停止注册/建应用），CloudBase 是官方指定迁移目标，国内直连且免费体验版
//  够用。服务端实现见 tools/cloudbase/ms-account.js（单端点 action 分发）：
//    POST <apiBase>  {action: register|login|heartbeat|pushPlaylist|pullPlaylist}
//
//  数据模型（CloudBase 控制台数据库集合，开发者可直接查看）：
//    ms_users     username/createdAt/lastActiveAt/deviceId/osVersion/...
//                 —— lastActiveAt 半小时内视为在线
//    ms_playlists owner/items/updatedAt —— 整包 last-write-wins
//
//  凭证来源（优先级）：Settings(用户可覆盖) > 编译期定义(MS_CLOUD_API_BASE，
//  发布机构建时由 cmake/secrets.cmake 提供，该文件不入库)。
// ============================================================================
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

namespace ms {

class CloudSync : public QObject
{
    Q_OBJECT

public:
    explicit CloudSync(QObject* parent = nullptr);

    // 从 Settings + 编译期常量解析云端 API 地址；未配置时所有上报静默
    // 跳过（纯本地模式）。
    void configureFromSettings();
    bool isConfigured() const { return !m_base.isEmpty(); }

    // 稳定的设备指纹（首次生成后写入 QSettings，长期不变）
    QString deviceId();
    QString osVersion() const;
    QString appVersion() const;

    // 启动自动登录后调用：用缓存的 token 恢复心跳（仅 isCloud 用户才调）。
    // token 失效时首次心跳会静默清除缓存，不影响本地功能。
    void resumeSession(const QString& username);

    // 注册上报：action=register 建账（失败降级，不影响本地注册）
    void reportRegister(const QString& username, const QString& pwdHash, const QString& avatar);

    // 登录上报：action=login 换取 token，并刷新 lastActiveAt
    void reportLogin(const QString& username, const QString& pwdHash);

    // 跨设备登录：本地库查无此账户时，到云端验证用户名+弱哈希。
    // 成功后发 cloudLoginResult(ok=true)，由 WebBridge 把云端账户镜像成本地账户。
    void cloudLogin(const QString& username, const QString& pwdHash);

    // 心跳：action=heartbeat 刷新 lastActiveAt（10 分钟一次）
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

    // 跨设备登录回执：ok 时带 username/avatar（云端档案），
    // 失败时 error 说明原因（含"云端无此账户/密码错误"）
    void cloudLoginResult(bool ok, const QString& username, const QString& pwdHash,
                          const QString& avatar, const QString& error);

private:
    void startHeartbeat();
    // 统一异步请求：POST <base>，body = {action, ...payload, token?}
    void callAction(const QString& action, const QJsonObject& payload, bool withToken);
    void handleSessionResponse(const QString& action, const QJsonObject& resp);
    QJsonObject baseProfile() const;

    QString  m_base;              // 云函数 HTTP 访问地址（单端点）
    QString  m_deviceId;
    QString  m_token;             // 云端会话令牌（登录/注册后缓存，30 天有效）
    QString  m_username;          // 当前云端账户名
    QTimer   m_heartbeat;         // 主线程定时器，网络动作仍投到线程池
};

} // namespace ms
