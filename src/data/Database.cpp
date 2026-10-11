// ============================================================================
//  src/data/Database.cpp
// ============================================================================
#include "data/Database.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSqlError>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QSet>
#include <QStringList>
#include <QThread>

namespace ms {

namespace {
const char* kCreateUsers = R"SQL(
CREATE TABLE IF NOT EXISTS users (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT    NOT NULL UNIQUE,
    pwd_hash    TEXT    NOT NULL,
    salt        TEXT    NOT NULL,
    avatar      TEXT    DEFAULT '',
    avatar_grad TEXT    DEFAULT '',
    cloud       INTEGER DEFAULT 0,
    created_at  TEXT    NOT NULL,
    last_seen   TEXT    NOT NULL
);
)SQL";

const char* kCreateLibrary = R"SQL(
CREATE TABLE IF NOT EXISTS library (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER NOT NULL,
    title      TEXT    NOT NULL,
    artist     TEXT    DEFAULT '',
    album      TEXT    DEFAULT '',
    genre      TEXT    DEFAULT '',
    year       INTEGER DEFAULT 0,
    track_no   INTEGER DEFAULT 0,
    duration   INTEGER DEFAULT 0,
    quality    TEXT    DEFAULT 'unknown',
    format     TEXT    DEFAULT '',
    path       TEXT    NOT NULL,
    cover      TEXT    DEFAULT '',
    size       INTEGER DEFAULT 0,
    added_at   TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_lib_user  ON library(user_id);
CREATE INDEX IF NOT EXISTS idx_lib_path  ON library(user_id, path);
)SQL";

const char* kCreatePlaylists = R"SQL(
CREATE TABLE IF NOT EXISTS playlists (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER NOT NULL,
    name       TEXT    NOT NULL,
    source_url TEXT    DEFAULT '',
    platform   TEXT    DEFAULT '',
    created_at TEXT    NOT NULL
);
CREATE TABLE IF NOT EXISTS playlist_tracks (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    playlist_id INTEGER NOT NULL,
    title       TEXT    NOT NULL,
    artist      TEXT    DEFAULT '',
    album       TEXT    DEFAULT '',
    duration    INTEGER DEFAULT 0,
    quality     TEXT    DEFAULT 'unknown',
    qualities   TEXT    DEFAULT '',
    platform_id TEXT    DEFAULT '',
    sort_order  INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_pltrack ON playlist_tracks(playlist_id);
)SQL";

const char* kCreateHistory = R"SQL(
CREATE TABLE IF NOT EXISTS download_history (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER NOT NULL,
    title      TEXT    NOT NULL,
    artist     TEXT    DEFAULT '',
    source     TEXT    DEFAULT '',
    quality    TEXT    DEFAULT 'unknown',
    status     TEXT    DEFAULT '',
    path       TEXT    DEFAULT '',
    size       INTEGER DEFAULT 0,
    error      TEXT    DEFAULT '',
    created_at TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_hist_user ON download_history(user_id);
)SQL";

QString iso() { return QDateTime::currentDateTime().toString(Qt::ISODate); }
} // namespace

// ---------------------------------------------------------------------------
Database& Database::instance()
{
    static Database s;
    return s;
}

QString Database::connNameForCurrentThread()
{
    // 每线程一个连接名：SQLite 的 QSqlDatabase 不支持跨线程复用
    return QStringLiteral("melodysync_%1")
        .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
}

QSqlDatabase Database::handle()
{
    const QString name = connNameForCurrentThread();
    if (QSqlDatabase::contains(name))
        return QSqlDatabase::database(name, /*open=*/true);

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(m_path);
    // 并发写入优化：WAL + 5 秒忙等待，避免多线程同时写库时报 database is locked
    db.open();
    QSqlQuery pragma(db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    ensureSchema(db);
    return db;
}

bool Database::open(const QString& path)
{
    QMutexLocker lock(&m_mutex);

    if (path.isEmpty()) {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.melodysync");
        QDir().mkpath(dir);
        m_path = QDir(dir).filePath(QStringLiteral("users.db"));
    } else {
        QDir().mkpath(QFileInfo(path).absolutePath());
        m_path = path;
    }

    QSqlDatabase db = handle();
    if (!db.isOpen()) {
        m_error = db.lastError().text();
        qWarning() << "[DB] 打开失败:" << m_error;
        return false;
    }
    qInfo() << "[DB] 已就绪:" << m_path;
    return true;
}

void Database::close()
{
    const QString name = connNameForCurrentThread();
    if (QSqlDatabase::contains(name)) {
        {
            QSqlDatabase db = QSqlDatabase::database(name, false);
            if (db.isOpen()) db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
}

bool Database::isOpen() { return handle().isOpen(); }

bool Database::ensureSchema(QSqlDatabase& db)
{
    const char* scripts[] = { kCreateUsers, kCreateLibrary, kCreatePlaylists, kCreateHistory };
    for (const char* sql : scripts) {
        // SQLite 的 QSqlQuery::exec() 一次**只能执行一条**语句，而下面每个
        // 脚本常量里既有 CREATE TABLE 又有 CREATE INDEX / 触发器。整段丢进去
        // 会失败：
        //     "not an error Unable to execute multiple statements at a time"
        // 这个报错特别容易被忽略 —— 它会打印在启动日志里、程序照常往下跑，
        // 但表根本没建出来，后续所有读写都是"看似成功实则空表"。
        //
        // 所以按分号拆开逐条执行；报错时把出问题的那条语句也打出来，
        // 免得只看到一个 SQLite 的英文错误无从下手。
        const QStringList stmts = QString::fromUtf8(sql)
                                      .split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString& piece : stmts) {
            const QString stmt = piece.trimmed();
            if (stmt.isEmpty())
                continue;
            QSqlQuery q(db);
            if (!q.exec(stmt)) {
                m_error = q.lastError().text();
                qWarning() << "[DB] 建表失败:" << m_error
                           << "\n  出错的语句:" << stmt.left(120);
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
bool Database::exec(const QString& sql, const QVariantList& binds)
{
    QSqlQuery q(handle());
    q.prepare(sql);
    for (const QVariant& v : binds) q.addBindValue(v);
    if (!q.exec()) {
        m_error = q.lastError().text();
        qWarning() << "[DB] exec 失败:" << m_error << sql.left(120);
        return false;
    }
    return true;
}

QSqlQuery Database::select(const QString& sql, const QVariantList& binds)
{
    QSqlQuery q(handle());
    q.prepare(sql);
    for (const QVariant& v : binds) q.addBindValue(v);
    if (!q.exec()) {
        m_error = q.lastError().text();
        qWarning() << "[DB] select 失败:" << m_error << sql.left(120);
    }
    return q;
}

// ---------------------------------------------------------------------------
// 用户
// ---------------------------------------------------------------------------
bool Database::insertUser(const QString& name, const QString& pwdHash, const QString& salt,
                          const QString& avatar, bool cloud, qint64* outId)
{
    QSqlQuery q(handle());
    q.prepare(QStringLiteral(
        "INSERT INTO users(name,pwd_hash,salt,avatar,cloud,created_at,last_seen)"
        " VALUES(?,?,?,?,?,?,?)"));
    q.addBindValue(name);
    q.addBindValue(pwdHash);
    q.addBindValue(salt);
    q.addBindValue(avatar);
    q.addBindValue(cloud ? 1 : 0);
    q.addBindValue(iso());
    q.addBindValue(iso());
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    if (outId) *outId = q.lastInsertId().toLongLong();
    return true;
}

bool Database::userExists(const QString& name)
{
    QSqlQuery q = select(QStringLiteral("SELECT 1 FROM users WHERE name=? LIMIT 1"), { name });
    return q.next();
}

static UserInfo readUser(const QSqlQuery& q)
{
    UserInfo u;
    u.id          = q.value(QStringLiteral("id")).toLongLong();
    u.name        = q.value(QStringLiteral("name")).toString();
    u.avatar      = q.value(QStringLiteral("avatar")).toString();
    u.avatarGrad  = q.value(QStringLiteral("avatar_grad")).toString();
    u.cloud       = q.value(QStringLiteral("cloud")).toInt() != 0;
    u.createdAt   = q.value(QStringLiteral("created_at")).toString();
    u.lastSeen    = q.value(QStringLiteral("last_seen")).toString();
    return u;
}

bool Database::findUserByName(const QString& name, UserInfo* out,
                              QString* outHash, QString* outSalt)
{
    QSqlQuery q = select(QStringLiteral("SELECT * FROM users WHERE name=?"), { name });
    if (!q.next()) return false;
    if (out)     *out = readUser(q);
    if (outHash) *outHash = q.value(QStringLiteral("pwd_hash")).toString();
    if (outSalt) *outSalt = q.value(QStringLiteral("salt")).toString();
    return true;
}

bool Database::findUserById(qint64 id, UserInfo* out)
{
    QSqlQuery q = select(QStringLiteral("SELECT * FROM users WHERE id=?"), { id });
    if (!q.next()) return false;
    if (out) *out = readUser(q);
    return true;
}

bool Database::updateUser(qint64 id, const QVariantMap& patch)
{
    static const QSet<QString> allowed = {
        QStringLiteral("name"), QStringLiteral("avatar"),
        QStringLiteral("avatar_grad"), QStringLiteral("cloud")
    };
    QStringList sets;
    QVariantList binds;
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
        if (!allowed.contains(it.key())) continue;      // 白名单，防止注入任意列
        sets << (it.key() + QStringLiteral("=?"));
        binds << it.value();
    }
    if (sets.isEmpty()) return true;
    binds << id;
    QSqlQuery q(handle());
    q.prepare(QStringLiteral("UPDATE users SET ") + sets.join(QLatin1Char(',')) +
              QStringLiteral(" WHERE id=?"));
    for (const QVariant& v : binds) q.addBindValue(v);
    if (!q.exec()) { m_error = q.lastError().text(); return false; }
    return true;
}

bool Database::updateUserPassword(qint64 id, const QString& pwdHash, const QString& salt)
{
    return exec(QStringLiteral("UPDATE users SET pwd_hash=?, salt=? WHERE id=?"),
                { pwdHash, salt, id });
}

bool Database::touchUser(qint64 id)
{
    return exec(QStringLiteral("UPDATE users SET last_seen=? WHERE id=?"), { iso(), id });
}

bool Database::deleteUser(qint64 id)
{
    // 单事务级联清理：用户 -> 音乐库 / 歌单 / 歌单曲目 / 下载历史
    QSqlDatabase db = handle();
    db.transaction();

    // 先删歌单曲目（依赖 playlist id）
    QSqlQuery sub(db);
    sub.prepare(QStringLiteral(
        "DELETE FROM playlist_tracks WHERE playlist_id IN"
        " (SELECT id FROM playlists WHERE user_id=?)"));
    sub.addBindValue(id);
    const bool ok1 = sub.exec();

    QSqlQuery q1(db); q1.prepare(QStringLiteral("DELETE FROM playlists WHERE user_id=?"));
    q1.addBindValue(id); const bool ok2 = q1.exec();

    QSqlQuery q2(db); q2.prepare(QStringLiteral("DELETE FROM library WHERE user_id=?"));
    q2.addBindValue(id); const bool ok3 = q2.exec();

    QSqlQuery q3(db); q3.prepare(QStringLiteral("DELETE FROM download_history WHERE user_id=?"));
    q3.addBindValue(id); const bool ok4 = q3.exec();

    QSqlQuery q4(db); q4.prepare(QStringLiteral("DELETE FROM users WHERE id=?"));
    q4.addBindValue(id); const bool ok5 = q4.exec();

    if (ok1 && ok2 && ok3 && ok4 && ok5) { db.commit(); return true; }
    m_error = QStringLiteral("删除用户数据失败");
    db.rollback();
    return false;
}

QVector<UserInfo> Database::allUsers()
{
    QVector<UserInfo> out;
    QSqlQuery q = select(QStringLiteral("SELECT * FROM users ORDER BY id"));
    while (q.next()) out.append(readUser(q));
    return out;
}

// ---------------------------------------------------------------------------
// 音乐库
// ---------------------------------------------------------------------------
qint64 Database::addLibraryItem(qint64 userId, const LibraryItem& item)
{
    QSqlQuery q(handle());
    q.prepare(QStringLiteral(
        "INSERT INTO library(user_id,title,artist,album,genre,year,track_no,duration,"
        "quality,format,path,cover,size,added_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(userId);
    q.addBindValue(item.title);
    q.addBindValue(item.artist);
    q.addBindValue(item.album);
    q.addBindValue(item.genre);
    q.addBindValue(item.year);
    q.addBindValue(item.trackNo);
    q.addBindValue(item.durationSec);
    q.addBindValue(qualityKey(item.quality));
    q.addBindValue(item.format);
    q.addBindValue(item.filePath);
    q.addBindValue(item.coverPath);
    q.addBindValue(item.sizeBytes);
    q.addBindValue(item.addedAt.isEmpty() ? iso() : item.addedAt);
    if (!q.exec()) { m_error = q.lastError().text(); return -1; }
    return q.lastInsertId().toLongLong();
}

bool Database::removeLibraryItem(qint64 userId, qint64 itemId)
{
    return exec(QStringLiteral("DELETE FROM library WHERE user_id=? AND id=?"), { userId, itemId });
}

bool Database::clearLibrary(qint64 userId)
{
    // 注意：只清空数据库记录，不删除磁盘上的音频文件 —— 删文件属于破坏性
    // 操作，必须由用户在文件管理器里显式完成，程序不代劳。
    return exec(QStringLiteral("DELETE FROM library WHERE user_id=?"), { userId });
}

QVector<LibraryItem> Database::library(qint64 userId)
{
    QVector<LibraryItem> out;
    QSqlQuery q = select(QStringLiteral("SELECT * FROM library WHERE user_id=? ORDER BY added_at DESC"),
                         { userId });
    while (q.next()) {
        LibraryItem it;
        it.id          = q.value(QStringLiteral("id")).toLongLong();
        it.title       = q.value(QStringLiteral("title")).toString();
        it.artist      = q.value(QStringLiteral("artist")).toString();
        it.album       = q.value(QStringLiteral("album")).toString();
        it.genre       = q.value(QStringLiteral("genre")).toString();
        it.year        = q.value(QStringLiteral("year")).toInt();
        it.trackNo     = q.value(QStringLiteral("track_no")).toInt();
        it.durationSec = q.value(QStringLiteral("duration")).toInt();
        it.quality     = qualityFromKey(q.value(QStringLiteral("quality")).toString());
        it.format      = q.value(QStringLiteral("format")).toString();
        it.filePath    = q.value(QStringLiteral("path")).toString();
        it.coverPath   = q.value(QStringLiteral("cover")).toString();
        it.sizeBytes   = q.value(QStringLiteral("size")).toLongLong();
        it.addedAt     = q.value(QStringLiteral("added_at")).toString();
        out.append(it);
    }
    return out;
}

bool Database::libraryHasFile(qint64 userId, const QString& path)
{
    QSqlQuery q = select(QStringLiteral("SELECT 1 FROM library WHERE user_id=? AND path=? LIMIT 1"),
                         { userId, path });
    return q.next();
}

QJsonObject Database::libraryStats(qint64 userId)
{
    QJsonObject root;
    QJsonArray byArtist, byAlbum, byQuality;
    qint64 total = 0;

    QSqlQuery q1 = select(QStringLiteral(
        "SELECT artist, COUNT(*) c, SUM(size) s FROM library WHERE user_id=?"
        " GROUP BY artist ORDER BY c DESC LIMIT 20"), { userId });
    while (q1.next()) {
        QJsonObject o;
        o[QStringLiteral("name")]  = q1.value(0).toString();
        o[QStringLiteral("count")] = q1.value(1).toInt();
        o[QStringLiteral("size")]  = q1.value(2).toLongLong();
        byArtist.append(o);
    }

    QSqlQuery q2 = select(QStringLiteral(
        "SELECT album, COUNT(*) c FROM library WHERE user_id=?"
        " GROUP BY album ORDER BY c DESC LIMIT 20"), { userId });
    while (q2.next()) {
        QJsonObject o;
        o[QStringLiteral("name")]  = q2.value(0).toString();
        o[QStringLiteral("count")] = q2.value(1).toInt();
        byAlbum.append(o);
    }

    QSqlQuery q3 = select(QStringLiteral(
        "SELECT quality, COUNT(*) c, SUM(size) s FROM library WHERE user_id=?"
        " GROUP BY quality"), { userId });
    while (q3.next()) {
        QJsonObject o;
        const QString key = q3.value(0).toString();
        o[QStringLiteral("name")]  = qualityLabel(qualityFromKey(key));
        o[QStringLiteral("key")]   = key;
        o[QStringLiteral("count")] = q3.value(1).toInt();
        o[QStringLiteral("size")]  = q3.value(2).toLongLong();
        byQuality.append(o);
    }

    QSqlQuery q4 = select(QStringLiteral("SELECT COUNT(*), IFNULL(SUM(size),0) FROM library WHERE user_id=?"),
                          { userId });
    if (q4.next()) {
        root[QStringLiteral("count")] = q4.value(0).toInt();
        total = q4.value(1).toLongLong();
    }
    root[QStringLiteral("totalSize")] = total;
    root[QStringLiteral("byArtist")]  = byArtist;
    root[QStringLiteral("byAlbum")]   = byAlbum;
    root[QStringLiteral("byQuality")] = byQuality;
    return root;
}

// ---------------------------------------------------------------------------
// 歌单
// ---------------------------------------------------------------------------
qint64 Database::addPlaylist(qint64 userId, const QString& name,
                             const QString& sourceUrl, const QString& platform)
{
    QSqlQuery q(handle());
    q.prepare(QStringLiteral(
        "INSERT INTO playlists(user_id,name,source_url,platform,created_at) VALUES(?,?,?,?,?)"));
    q.addBindValue(userId);
    q.addBindValue(name);
    q.addBindValue(sourceUrl);
    q.addBindValue(platform);
    q.addBindValue(iso());
    if (!q.exec()) { m_error = q.lastError().text(); return -1; }
    return q.lastInsertId().toLongLong();
}

bool Database::removePlaylist(qint64 userId, qint64 playlistId)
{
    exec(QStringLiteral("DELETE FROM playlist_tracks WHERE playlist_id=?"), { playlistId });
    return exec(QStringLiteral("DELETE FROM playlists WHERE user_id=? AND id=?"),
                { userId, playlistId });
}

bool Database::addPlaylistTracks(qint64 playlistId, const QVector<PlaylistTrack>& tracks)
{
    QSqlDatabase db = handle();
    db.transaction();
    int order = 0;
    for (const PlaylistTrack& t : tracks) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT INTO playlist_tracks(playlist_id,title,artist,album,duration,quality,"
            "qualities,platform_id,sort_order) VALUES(?,?,?,?,?,?,?,?,?)"));
        q.addBindValue(playlistId);
        q.addBindValue(t.title);
        q.addBindValue(t.artist);
        q.addBindValue(t.album);
        q.addBindValue(t.durationSec);
        q.addBindValue(qualityKey(t.bestQuality));
        q.addBindValue(t.qualities.join(QLatin1Char(',')));
        q.addBindValue(t.platformId);
        q.addBindValue(order++);
        if (!q.exec()) { db.rollback(); m_error = q.lastError().text(); return false; }
    }
    db.commit();
    return true;
}

QJsonArray Database::playlists(qint64 userId)
{
    QJsonArray arr;
    QSqlQuery q = select(QStringLiteral(
        "SELECT p.id,p.name,p.platform,p.source_url,p.created_at,"
        " (SELECT COUNT(*) FROM playlist_tracks t WHERE t.playlist_id=p.id) c"
        " FROM playlists p WHERE p.user_id=? ORDER BY p.id DESC"), { userId });
    while (q.next()) {
        QJsonObject o;
        o[QStringLiteral("id")]        = q.value(0).toLongLong();
        o[QStringLiteral("name")]      = q.value(1).toString();
        o[QStringLiteral("platform")]  = q.value(2).toString();
        o[QStringLiteral("sourceUrl")] = q.value(3).toString();
        o[QStringLiteral("createdAt")] = q.value(4).toString();
        o[QStringLiteral("count")]     = q.value(5).toInt();
        arr.append(o);
    }
    return arr;
}

QJsonObject Database::playlistDetail(qint64 playlistId)
{
    QJsonObject root;
    QSqlQuery q = select(QStringLiteral("SELECT * FROM playlists WHERE id=?"), { playlistId });
    if (q.next()) {
        root[QStringLiteral("id")]        = q.value(QStringLiteral("id")).toLongLong();
        root[QStringLiteral("name")]      = q.value(QStringLiteral("name")).toString();
        root[QStringLiteral("platform")]  = q.value(QStringLiteral("platform")).toString();
        root[QStringLiteral("sourceUrl")] = q.value(QStringLiteral("source_url")).toString();
    }
    QJsonArray songs;
    QSqlQuery t = select(QStringLiteral(
        "SELECT * FROM playlist_tracks WHERE playlist_id=? ORDER BY sort_order"), { playlistId });
    while (t.next()) {
        PlaylistTrack pt;
        pt.title       = t.value(QStringLiteral("title")).toString();
        pt.artist      = t.value(QStringLiteral("artist")).toString();
        pt.album       = t.value(QStringLiteral("album")).toString();
        pt.durationSec = t.value(QStringLiteral("duration")).toInt();
        pt.bestQuality = qualityFromKey(t.value(QStringLiteral("quality")).toString());
        pt.platformId  = t.value(QStringLiteral("platform_id")).toString();
        const QString qs = t.value(QStringLiteral("qualities")).toString();
        if (!qs.isEmpty()) pt.qualities = qs.split(QLatin1Char(','), Qt::SkipEmptyParts);
        songs.append(pt.toJson());
    }
    root[QStringLiteral("songs")] = songs;
    root[QStringLiteral("count")] = songs.size();
    return root;
}

// ---------------------------------------------------------------------------
// 下载历史
// ---------------------------------------------------------------------------
bool Database::addHistory(qint64 userId, const DownloadTask& task)
{
    QSqlQuery q(handle());
    q.prepare(QStringLiteral(
        "INSERT INTO download_history(user_id,title,artist,source,quality,status,path,size,error,created_at)"
        " VALUES(?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(userId);
    q.addBindValue(task.title);
    q.addBindValue(task.artist);
    q.addBindValue(task.sourceId);
    q.addBindValue(qualityKey(task.quality));
    q.addBindValue(statusKey(task.status));
    q.addBindValue(task.savePath);
    q.addBindValue(task.totalBytes);
    q.addBindValue(task.error);
    q.addBindValue(iso());
    return q.exec();
}

QJsonArray Database::history(qint64 userId, int limit)
{
    QJsonArray arr;
    QSqlQuery q = select(QStringLiteral(
        "SELECT title,artist,source,quality,status,path,size,error,created_at"
        " FROM download_history WHERE user_id=? ORDER BY id DESC LIMIT ?"), { userId, limit });
    while (q.next()) {
        QJsonObject o;
        QString name = q.value(9).toString();
        o[QStringLiteral("name")]    = q.value(0).toString();
        o[QStringLiteral("artist")]  = q.value(1).toString();
        o[QStringLiteral("src")]     = q.value(2).toString();
        o[QStringLiteral("qual")]    = qualityLabel(qualityFromKey(q.value(3).toString()));
        o[QStringLiteral("status")]  = q.value(4).toString();
        o[QStringLiteral("path")]    = q.value(5).toString();
        o[QStringLiteral("size")]    = q.value(6).toLongLong() / 1024.0 / 1024.0;
        o[QStringLiteral("error")]   = q.value(7).toString();
        o[QStringLiteral("time")]    = q.value(8).toString();
        arr.append(o);
    }
    return arr;
}

bool Database::clearHistory(qint64 userId)
{
    return exec(QStringLiteral("DELETE FROM download_history WHERE user_id=?"), { userId });
}

} // namespace ms
