// ============================================================================
//  src/auth/UserManager.h
//  本地账户管理：注册 / 登录 / 自动登录 / 修改资料 / 删除账户。
//
//  密码策略（见 cpp 内详细注释）：
//    前端传入 pwdHash（JS 侧弱哈希，仅用于"不传明文"）
//    后端再叠加 随机盐(SHA-256) + 12000 轮迭代 后落库，绝不存明文。
// ============================================================================
#pragma once

#include "core/Types.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace ms {

class UserManager : public QObject
{
    Q_OBJECT

public:
    explicit UserManager(QObject* parent = nullptr);

    bool init();

    // 全部返回可直接下发前端的 JSON：{ok:bool, error:string, user:{}}
    QJsonObject registerUser(const QString& name, const QString& pwdHash,
                             const QString& avatar, bool isCloud);
    QJsonObject loginUser(const QString& name, const QString& pwdHash);
    QJsonObject logoutUser();
    QJsonObject currentUser() const;
    QJsonObject updateUser(const QJsonObject& patch);
    QJsonObject deleteUser();

    // 自动登录：从上一次的 session 恢复
    bool restoreSession();

    qint64  currentId() const { return m_currentId; }
    QString currentName() const { return m_current.name; }
    bool    loggedIn() const { return m_currentId > 0; }

    // 密码学工具（公开出来给 CloudSync / 单元测试复用）
    static QString makeSalt(int bytes = 16);
    // iterations 传 0（省略）时用 kIterations。实现里本来就有
    // 「iterations <= 0 就取默认轮数」的兜底，但声明处没给默认值，
    // 于是所有 hashPassword(pwdHash, salt) 的两参调用都编译不过。
    static QString hashPassword(const QString& pwdHash, const QString& salt, int iterations = 0);

    // 昵称合法性：2-20 字符
    static QString validateName(const QString& name);

signals:
    void userChanged(const QString& userJson);

private:
    void persistSession(qint64 id);
    void clearSession();
    QString avatarGradient(const QString& avatar) const;

    UserInfo m_current;
    qint64   m_currentId = 0;
};

} // namespace ms
