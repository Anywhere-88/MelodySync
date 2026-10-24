// ============================================================================
//  src/bridge/WebBridge.h
//  JS ↔ C++ 的唯一通道（QWebChannel）。
//
//  ★ 关于"返回 JSON"与"异步"的重要约定
//  ---------------------------------------------------------------------------
//  QWebChannel 的 Q_INVOKABLE 是在 **UI 线程** 上执行的。因此凡是可能耗时
//  超过几十毫秒的操作（网络搜索、歌单解析、下载、设备同步），一律不能
//  同步返回结果 —— 那会把界面卡死。
//
//  本桥接层的统一模式：
//     1) 前端调用 xxx(...) → 立即返回 {"ok":true,"pending":true,"ticket":"..."}
//     2) 真正的活干在后台线程里
//     3) 完成后通过对应信号把结果 JSON 推回前端：
//          aiSearch      → aiSearchDone(json)
//          parsePlaylist → playlistParsed(json)
//          startSync     → syncProgress(...) + syncDone(json)
//  前端只需要 await 一次信号即可，交互上等价于 Promise。
//
//  只有纯本地的轻量读取（getCurrentUser / getQueue / getSettings / getLibrary…）
//  才同步返回，因为它们读的是内存或本地 SQLite，耗时在毫秒级。
// ============================================================================
#pragma once

#include "core/Async.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <memory>

namespace ms {

class CloudSync;
class DeviceManager;
class DownloadEngine;
class PlaylistParser;
class SearchAggregator;
class UserManager;

class WebBridge : public QObject
{
    Q_OBJECT

public:
    explicit WebBridge(QObject* parent = nullptr);
    ~WebBridge() override;

    // 由 main.cpp / MainWindow 在窗口就绪后调用
    void bootstrap();

    // 供 MainWindow 把 playDemo 的请求转给前端（或反之）
    void notifyFrontend(const QString& event, const QString& payload);

public slots:
    // ------------------------------------------------------------------ 账户
    Q_INVOKABLE QString registerUser(const QString& name, const QString& pwdHash,
                                     const QString& avatar, bool isCloud);
    Q_INVOKABLE QString loginUser(const QString& name, const QString& pwdHash);
    Q_INVOKABLE void    logoutUser();
    Q_INVOKABLE QString getCurrentUser();
    Q_INVOKABLE QString updateUser(const QString& patchJson);
    Q_INVOKABLE QString deleteUser();

    // 云端歌单同步（LeanCloud，无服务器）。结果经 cloudPlaylistPulled 信号
    // 回前端（拉取结果 / 推送失败提示都走它，保证任何路径都有回包）。
    Q_INVOKABLE void    cloudPushPlaylist(const QString& itemsJson);
    Q_INVOKABLE void    cloudPullPlaylist();

    // ------------------------------------------------------------------ 歌单
    Q_INVOKABLE QString parsePlaylist(const QString& url, const QString& platform);
    Q_INVOKABLE QString qrLoginCreate();
    Q_INVOKABLE QString qrLoginPoll(const QString& unikey);
    Q_INVOKABLE QString getPlaylists();
    Q_INVOKABLE QString getPlaylistDetail(int playlistId);
    Q_INVOKABLE QString deletePlaylist(int playlistId);
    Q_INVOKABLE QString savePlaylist(const QString& playlistJson);

    // ------------------------------------------------------------------ 本地播放
    // 本地音频文件 → <audio> 可用的 http://127.0.0.1:port/a/<token> 地址。
    // 为什么不直接用 file://：qrc 页面喂本地文件会被 Chromium 拦（跨源子资源），
    // 实测表现为"文件明明在，播放失败"。路径非法/文件不存在时返回空串。
    Q_INVOKABLE QString audioUrl(const QString& absolutePath);

    // ------------------------------------------------------------------ 歌词
    // 异步取歌词：本地同名 .lrc 优先 → 网易云（搜索匹配 songId → lyric API 拿 LRC）。
    // HTTP 不能在 UI 线程发（QWebChannel 的 Q_INVOKABLE 跑在 UI 线程），
    // 所以结果经 lyricsReady 信号回推。任何路径（包括失败）都保证发信号。
    Q_INVOKABLE void fetchLyrics(const QString& title, const QString& artist,
                                 const QString& localPath);

    // ------------------------------------------------------------------ 音乐库
    // 删除歌曲文件：移入系统回收站（不是直接抹掉，可恢复），同时移除库记录。
    Q_INVOKABLE QString deleteLibraryFile(int itemId);

    // 确保目录存在且可写（设置下载目录时前端先调它验证）
    Q_INVOKABLE QString ensureDir(const QString& path);

    // ------------------------------------------------------------------ AI 搜索
    Q_INVOKABLE QString aiSearch(const QString& query, const QString& sourcesJson);

    // 向当前配置的 AI 服务商发一次最小请求，验证「地址 / Key / 模型名」是否都对。
    // 结果走 aiTestResult 信号。
    //
    // 为什么需要它：这三个东西错任何一个，AI 搜索都只是"静静回落成规则解析"，
    // 用户看不出区别。用一个真实请求去探，比任何本地格式校验都可靠。
    Q_INVOKABLE QString testAiConnection();
    Q_INVOKABLE QString getSources();
    Q_INVOKABLE void    cancelSearch();

    // ------------------------------------------------------------------ 下载
    Q_INVOKABLE void    addToQueue(const QString& songsJson);
    Q_INVOKABLE void    startAll();
    Q_INVOKABLE void    pauseAll();
    Q_INVOKABLE void    clearDone();
    Q_INVOKABLE void    cleanQueue();
    Q_INVOKABLE void    retryFailed();
    Q_INVOKABLE void    cancelTask(const QString& taskId);
    Q_INVOKABLE void    removeTask(const QString& taskId);
    Q_INVOKABLE QString getQueue();

    // ------------------------------------------------------------------ 音乐库
    Q_INVOKABLE QString getLibrary();
    Q_INVOKABLE QString getAnalysis();
    Q_INVOKABLE QString getHistory();
    Q_INVOKABLE void    clearHistory();
    Q_INVOKABLE void    removeLibraryItem(int itemId);
    Q_INVOKABLE void    clearLibrary();

    // ------------------------------------------------------------------ 设备
    Q_INVOKABLE QString getDevices();
    Q_INVOKABLE void    refreshDevices();
    Q_INVOKABLE void    selectDevice(const QString& deviceId);
    // 返回 JSON 串（与其它桥接方法一致）：立即回 {ok:true,pending:true}，
    // 真正的结果通过 syncProgress / syncDone 信号下发。
    // 之前声明成 void 与实现（返回 QString）冲突，导致 C2371/C2556。
    Q_INVOKABLE QString startSync(const QString& deviceId, const QString& scope);
    Q_INVOKABLE void    cancelSync();
    Q_INVOKABLE QString planSync(const QString& deviceId);
    // 选择性同步的计划版：scope = "all" 或 JSON {"ids":[音乐库 id, ...]}。
    // 与 startSync 的 scope 同一契约 —— 先让用户看到"勾的这些要传多少"，再真正开始。
    Q_INVOKABLE QString planSyncEx(const QString& deviceId, const QString& scope);

    // ------------------------------------------------------------------ 设置
    Q_INVOKABLE QString getSettings();
    Q_INVOKABLE QString saveSettings(const QString& json);

    // ------------------------------------------------------------------ 播放
    Q_INVOKABLE void    playDemo(const QString& songId);
    Q_INVOKABLE void    togglePlay();
    Q_INVOKABLE QString openInExplorer(const QString& path);
    Q_INVOKABLE QString appInfo();

signals:
    // 数据推送（全部是 JSON 字符串）
    void queueUpdated(const QString& json);
    void libraryUpdated(const QString& json);
    void devicesUpdated(const QString& json);

    // 进度类
    void downloadProgress(const QString& id, int percent, qint64 speed);
    void syncProgress(int percent, int current, int total);
    void aiSearchProgress(const QString& sourceId, const QString& status, int count);

    // 结果类（异步操作的回执）
    void aiSearchDone(const QString& json);
    void aiTestResult(const QString& json);     // AI 连通性自测（testAiConnection 的回执）
    void aiSourceHits(const QString& sourceId, const QString& hitsJson);
    void playlistParsed(const QString& json);
    void playlistsLoaded(const QString& json);
    void playlistDetailLoaded(const QString& json);
    void playlistSaved(const QString& json);
    void qrLoginResult(const QString& json);    // 扫码登录：创建/轮询共用
    void syncDone(const QString& json);
    void userChanged(const QString& json);
    void lyricsReady(const QString& json);      // 歌词获取完成（fetchLyrics 的回执）
    void authResult(const QString& json);       // 注册/登录/改资料/删账户的回执
    void cloudPlaylistPulled(const QString& json); // 云端歌单拉取/推送的回执（LeanCloud）
    void settingsLoaded(const QString& json);
    void sourcesLoaded(const QString& json);
    void analysisLoaded(const QString& json);
    void historyLoaded(const QString& json);
    void appInfoLoaded(const QString& json);

    // 轻提示：type = "" | "w" | "e"
    void toast(const QString& message, const QString& type);

private:
    void wireSignals();
    qint64 currentUserId() const;

    std::unique_ptr<UserManager>       m_users;
    std::unique_ptr<CloudSync>         m_cloud;
    std::unique_ptr<SearchAggregator>  m_search;
    std::unique_ptr<PlaylistParser>    m_playlists;
    std::unique_ptr<DownloadEngine>    m_downloads;
    std::unique_ptr<DeviceManager>     m_devices;

    struct SyncState;
    std::shared_ptr<SyncState> m_sync;
    CancelTokenPtr m_searchCancel;
};

} // namespace ms
