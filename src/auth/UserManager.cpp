// ============================================================================
//  src/auth/UserManager.cpp
//
//  ★ 关键算法：密码哈希（加盐 + 迭代 SHA-256）
//  ---------------------------------------------------------------------------
//  为什么不能直接存 SHA256(password)：
//    1) 彩虹表攻击：单一 SHA256 的映射是固定的，网上有 TB 级预计算表；
//    2) 相同密码产生相同摘要：攻击者一眼就能看出哪些用户用了同一个密码；
//    3) GPU 暴力破解：现代显卡每秒可算上百亿次 SHA256。
//
//  本工程的方案（三层防护）：
//    第 1 层（客户端）：前端先用 djb2 变体做一次弱哈希，保证"明文不出进程"。
//    第 2 层（加盐）：服务端为每个账户生成 16 字节随机盐（QRandomGenerator::system），
//                     盐与摘要一起存库。即使两个用户密码相同，落库摘要也不同，
//                     彩虹表也随之失效（必须为每个盐重建表）。
//    第 3 层（迭代）：把 hash = SHA256(salt || hash) 重复 12000 轮。
//                     破解成本从 O(1) 提升到 O(12000)，把暴力破解从"秒级"推到
//                     "天级"，而正常登录只多花几毫秒。
//
//  未使用 bcrypt/scrypt/Argon2 的原因：Qt 原生不提供，引入额外加密库会显著
//  增加打包体积；在"纯本地账户 + 可选云端"的场景下，上述方案已经足够。
//  若将来要对外开放注册，建议换成 Argon2id。
// ============================================================================
#include "auth/UserManager.h"

#include "data/Database.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QVariantMap>

namespace ms {

namespace {
const int kIterations = 12000;      // 迭代轮数：兼顾安全与登录延迟（约 5-10ms）
const char* kSessionKey = "auth/sessionUserId";
const char* kSessionName = "auth/sessionName";
}

UserManager::UserManager(QObject* parent) : QObject(parent) {}

bool UserManager::init()
{
    // Database 由 main.cpp 提前 open()，这里只做可用性检查
    if (!Database::instance().isOpen()) {
        qWarning() << "[Auth] 数据库不可用，账户功能将不可用";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 盐：16 字节密码学安全随机数，十六进制存储（32 个字符）
// ---------------------------------------------------------------------------
QString UserManager::makeSalt(int bytes)
{
    QByteArray buf(bytes, Qt::Uninitialized);
    // QRandomGenerator::system() 在 Windows 下取 CryptGenRandom，是密码学安全源
    QRandomGenerator::system()->generate(buf.begin(), buf.end());
    return QString::fromLatin1(buf.toHex());
}

// ---------------------------------------------------------------------------
// 迭代加盐哈希
// ---------------------------------------------------------------------------
QString UserManager::hashPassword(const QString& pwdHash, const QString& salt, int iterations)
{
    if (iterations <= 0) iterations = kIterations;

    // 第一轮：salt 前置，避免"密码前缀碰撞"
    QByteArray data = (salt + pwdHash).toUtf8();
    QByteArray digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256);

    for (int i = 1; i < iterations; ++i) {
        // 后续轮：把上一轮摘要与盐再混合，防止长度扩展攻击
        QByteArray round = digest;
        round.append(salt.toUtf8());
        digest = QCryptographicHash::hash(round, QCryptographicHash::Sha256);
    }
    return QString::fromLatin1(digest.toHex());
}

QString UserManager::validateName(const QString& name)
{
    const QString n = name.trimmed();
    if (n.size() < 2)  return QStringLiteral("昵称至少 2 个字符");
    if (n.size() > 20) return QStringLiteral("昵称最多 20 个字符");
    if (n.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f]"))))
        return QStringLiteral("昵称包含非法字符");
    return QString();
}

QString UserManager::avatarGradient(const QString& avatar) const
{
    // 与前端 GRADS 保持一致：同一个 emoji 永远配同一条渐变，避免头像跳色
    static const QString grads[] = {
        QStringLiteral("linear-gradient(135deg,#5b8cff,#8b5bff)"),
        QStringLiteral("linear-gradient(135deg,#00e0b8,#00b8d4)"),
        QStringLiteral("linear-gradient(135deg,#ff6b9d,#f5a524)"),
        QStringLiteral("linear-gradient(135deg,#8b5bff,#ff6b9d)"),
        QStringLiteral("linear-gradient(135deg,#00b8d4,#5b8cff)"),
        QStringLiteral("linear-gradient(135deg,#f5a524,#00e0b8)")
    };
    static const QString avatars[] = {
        QStringLiteral("🎵"), QStringLiteral("🎧"), QStringLiteral("🎸"), QStringLiteral("🎹"),
        QStringLiteral("🎤"), QStringLiteral("🌙"), QStringLiteral("⭐"), QStringLiteral("🔥"),
        QStringLiteral("💫"), QStringLiteral("🚀"), QStringLiteral("🎯"), QStringLiteral("💎")
    };
    int idx = 0;
    for (int i = 0; i < 12; ++i) if (avatars[i] == avatar) { idx = i; break; }
    return grads[idx % 6];
}

// ---------------------------------------------------------------------------
QJsonObject UserManager::registerUser(const QString& name, const QString& pwdHash,
                                      const QString& avatar, bool isCloud)
{
    QJsonObject res;
    const QString clean = name.trimmed();

    const QString err = validateName(clean);
    if (!err.isEmpty()) { res[QStringLiteral("ok")] = false; res[QStringLiteral("error")] = err; return res; }
    if (pwdHash.size() < 6) {
        res[QStringLiteral("ok")] = false;
        res[QStringLiteral("error")] = QStringLiteral("密码强度不足");
        return res;
    }
    if (Database::instance().userExists(clean)) {
        res[QStringLiteral("ok")] = false;
        res[QStringLiteral("error")] = QStringLiteral("该账户名称已被使用");
        return res;
    }

    const QString salt = makeSalt();
    const QString hash = hashPassword(pwdHash, salt);

    qint64 id = 0;
    if (!Database::instance().insertUser(clean, hash, salt, avatar, isCloud, &id)) {
        res[QStringLiteral("ok")] = false;
        res[QStringLiteral("error")] = QStringLiteral("写入账户失败：") + Database::instance().lastError();
        return res;
    }

    UserInfo u;
    Database::instance().findUserById(id, &u);
    u.avatarGrad = avatarGradient(u.avatar);

    m_current = u;
    m_currentId = id;
    persistSession(id);

    res[QStringLiteral("ok")]   = true;
    res[QStringLiteral("user")] = u.toJson();
    emit userChanged(QString::fromUtf8(QJsonDocument(u.toJson()).toJson(QJsonDocument::Compact)));
    return res;
}

QJsonObject UserManager::loginUser(const QString& name, const QString& pwdHash)
{
    QJsonObject res;
    UserInfo u;
    QString storedHash, salt;
    if (!Database::instance().findUserByName(name.trimmed(), &u, &storedHash, &salt)) {
        res[QStringLiteral("ok")] = false;
        res[QStringLiteral("error")] = QStringLiteral("未找到该账户");
        return res;
    }

    // 用相同的盐与轮数重算，再定长比较（QCryptographicHash 输出等长，可直接比）
    const QString candidate = hashPassword(pwdHash, salt);
    if (candidate != storedHash) {
        res[QStringLiteral("ok")] = false;
        res[QStringLiteral("error")] = QStringLiteral("密码错误");
        return res;
    }

    Database::instance().touchUser(u.id);
    Database::instance().findUserById(u.id, &u);
    u.avatarGrad = avatarGradient(u.avatar);

    m_current = u;
    m_currentId = u.id;
    persistSession(u.id);

    res[QStringLiteral("ok")]   = true;
    res[QStringLiteral("user")] = u.toJson();
    emit userChanged(QString::fromUtf8(QJsonDocument(u.toJson()).toJson(QJsonDocument::Compact)));
    return res;
}

QJsonObject UserManager::logoutUser()
{
    m_currentId = 0;
    m_current = UserInfo();
    clearSession();
    emit userChanged(QStringLiteral("{}"));
    QJsonObject res;
    res[QStringLiteral("ok")] = true;
    return res;
}

QJsonObject UserManager::currentUser() const
{
    if (m_currentId <= 0) return QJsonObject();
    UserInfo u = m_current;
    u.avatarGrad = const_cast<UserManager*>(this)->avatarGradient(u.avatar);
    return u.toJson();
}

QJsonObject UserManager::updateUser(const QJsonObject& patch)
{
    QJsonObject res;
    if (m_currentId <= 0) {
        res[QStringLiteral("ok")] = false;
        res[QStringLiteral("error")] = QStringLiteral("未登录");
        return res;
    }

    // 改密码：单独处理（需要重新生成盐）
    const QString newPwd = patch.value(QStringLiteral("newPasswordHash")).toString();
    if (!newPwd.isEmpty()) {
        const QString salt = makeSalt();
        const QString hash = hashPassword(newPwd, salt);
        Database::instance().updateUserPassword(m_currentId, hash, salt);
    }

    QVariantMap cols;
    if (patch.contains(QStringLiteral("name"))) {
        const QString n = patch.value(QStringLiteral("name")).toString().trimmed();
        const QString err = validateName(n);
        if (!err.isEmpty()) { res[QStringLiteral("ok")] = false; res[QStringLiteral("error")] = err; return res; }
        if (n != m_current.name && Database::instance().userExists(n)) {
            res[QStringLiteral("ok")] = false;
            res[QStringLiteral("error")] = QStringLiteral("该昵称已被占用");
            return res;
        }
        cols[QStringLiteral("name")] = n;
    }
    if (patch.contains(QStringLiteral("avatar"))) {
        const QString a = patch.value(QStringLiteral("avatar")).toString();
        cols[QStringLiteral("avatar")] = a;
        cols[QStringLiteral("avatar_grad")] = avatarGradient(a);
    }
    if (patch.contains(QStringLiteral("cloud")))
        cols[QStringLiteral("cloud")] = patch.value(QStringLiteral("cloud")).toBool() ? 1 : 0;

    if (!cols.isEmpty() && !Database::instance().updateUser(m_currentId, cols)) {
        res[QStringLiteral("ok")] = false;
        res[QStringLiteral("error")] = QStringLiteral("更新失败");
        return res;
    }

    Database::instance().findUserById(m_currentId, &m_current);
    m_current.avatarGrad = avatarGradient(m_current.avatar);

    res[QStringLiteral("ok")]   = true;
    res[QStringLiteral("user")] = m_current.toJson();
    emit userChanged(QString::fromUtf8(QJsonDocument(m_current.toJson()).toJson(QJsonDocument::Compact)));
    return res;
}

QJsonObject UserManager::deleteUser()
{
    QJsonObject res;
    if (m_currentId <= 0) {
        res[QStringLiteral("ok")] = false;
        res[QStringLiteral("error")] = QStringLiteral("未登录");
        return res;
    }
    const qint64 id = m_currentId;

    // 先清会话，避免删除中途失败留下"已登录但用户不存在"的状态
    logoutUser();

    if (!Database::instance().deleteUser(id)) {
        res[QStringLiteral("ok")] = false;
        res[QStringLiteral("error")] = QStringLiteral("删除账户数据失败");
        return res;
    }
    res[QStringLiteral("ok")] = true;
    return res;
}

// ---------------------------------------------------------------------------
bool UserManager::restoreSession()
{
    QSettings s;
    const qint64 id = s.value(QLatin1String(kSessionKey), 0).toLongLong();
    if (id <= 0) return false;

    UserInfo u;
    if (!Database::instance().findUserById(id, &u)) {
        clearSession();       // 账户已被删除
        return false;
    }
    u.avatarGrad = avatarGradient(u.avatar);
    m_current = u;
    m_currentId = id;
    Database::instance().touchUser(id);
    return true;
}

void UserManager::persistSession(qint64 id)
{
    QSettings s;
    s.setValue(QLatin1String(kSessionKey), id);
    s.setValue(QLatin1String(kSessionName), m_current.name);
    s.sync();
}

void UserManager::clearSession()
{
    QSettings s;
    s.remove(QLatin1String(kSessionKey));
    s.remove(QLatin1String(kSessionName));
    s.sync();
}

} // namespace ms
