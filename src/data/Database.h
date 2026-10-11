// ============================================================================
//  src/data/Database.h
//  全局 SQLite 访问层。
//
//  并发要点：QSqlDatabase 不允许跨线程共享连接。这里的做法是
//  "一个线程一个连接"，连接名带上线程 ID；每个后台线程首次访问时
//  自动建立自己的连接并共享同一份 db 文件（SQLite 自身用文件锁串行化写入）。
// ============================================================================
#pragma once

#include "core/Types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace ms {

class Database
{
public:
    static Database& instance();

    // path 为空时自动使用 QStandardPaths::AppDataLocation/users.db
    bool open(const QString& path = QString());
    void close();
    QString path() const { return m_path; }
    QString lastError() const { return m_error; }
    bool isOpen();

    // ---------------- 通用执行 ----------------
    bool      exec(const QString& sql, const QVariantList& binds = QVariantList());
    QSqlQuery select(const QString& sql, const QVariantList& binds = QVariantList());

    // ---------------- 用户 ----------------
    bool     insertUser(const QString& name, const QString& pwdHash, const QString& salt,
                        const QString& avatar, bool cloud, qint64* outId = nullptr);
    bool     findUserByName(const QString& name, UserInfo* out, QString* outHash, QString* outSalt);
    bool     findUserById(qint64 id, UserInfo* out);
    bool     userExists(const QString& name);
    bool     updateUser(qint64 id, const QVariantMap& patch);
    bool     updateUserPassword(qint64 id, const QString& pwdHash, const QString& salt);
    bool     touchUser(qint64 id);
    bool     deleteUser(qint64 id);          // 级联删除该用户全部数据
    QVector<UserInfo> allUsers();

    // ---------------- 音乐库 ----------------
    qint64   addLibraryItem(qint64 userId, const LibraryItem& item);
    bool     removeLibraryItem(qint64 userId, qint64 itemId);
    bool     clearLibrary(qint64 userId);      // 只清记录，不动磁盘文件
    QVector<LibraryItem> library(qint64 userId);
    QJsonObject libraryStats(qint64 userId);   // 存储分析：按歌手/专辑/音质聚合
    bool     libraryHasFile(qint64 userId, const QString& path);

    // ---------------- 我的歌单 ----------------
    qint64   addPlaylist(qint64 userId, const QString& name, const QString& sourceUrl,
                         const QString& platform);
    bool     removePlaylist(qint64 userId, qint64 playlistId);
    bool     addPlaylistTracks(qint64 playlistId, const QVector<PlaylistTrack>& tracks);
    QJsonArray playlists(qint64 userId);
    QJsonObject playlistDetail(qint64 playlistId);

    // ---------------- 下载历史 ----------------
    bool     addHistory(qint64 userId, const DownloadTask& task);
    QJsonArray history(qint64 userId, int limit = 200);
    bool     clearHistory(qint64 userId);

private:
    Database() = default;

    QSqlDatabase handle();                  // 取（必要时创建）当前线程连接
    static QString connNameForCurrentThread();
    bool ensureSchema(QSqlDatabase& db);

    QString m_path;
    QString m_error;
    mutable QMutex m_mutex;
};

} // namespace ms
