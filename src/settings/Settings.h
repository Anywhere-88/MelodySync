// ============================================================================
//  src/settings/Settings.h
//  QSettings 封装：所有可配置项的读写都从这里走，避免散落的魔法字符串。
//  存储位置：%APPDATA%/MelodySync/settings.ini（IniFormat，方便用户手改）
// ============================================================================
#pragma once

#include <QJsonObject>
#include <QSettings>
#include <QString>

namespace ms {

class Settings
{
public:
    static Settings& instance();

    // 与前端交互：前端 getSettings() 拿到扁平 JSON，saveSettings(json) 回写
    QJsonObject all() const;
    bool        save(const QJsonObject& patch);

    // 一次性配置迁移（新增搜索源等）。只在启动时从主线程调一次。
    void    migrate();

    // ---------------- 下载 ----------------
    QString downloadDir() const;
    void    setDownloadDir(const QString& dir);
    int     concurrent() const;                 // 并发数 1-8，默认 3
    void    setConcurrent(int n);
    int     retries() const;                    // 失败重试次数，默认 2
    void    setRetries(int n);
    QString preferQuality() const;              // flac / 320 / 128

    // ---------------- 元数据 ----------------
    bool    writeMetadata() const;              // 写入标签，默认开
    bool    embedCover() const;                 // 嵌入封面，默认开
    bool    fetchLyrics() const;                // 下载 .lrc，默认关
    QString lyricsDir() const;

    // ---------------- AI ----------------
    QString aiBaseUrl() const;                  // OpenAI 兼容 base，默认 https://api.openai.com/v1
    QString aiApiKey() const;
    QString aiModel() const;                    // 默认 gpt-4o-mini
    QString aiProvider() const;                 // openai / claude / glm / custom
    int     aiTimeoutMs() const;
    bool    aiThinkingVisible() const;          // 前端"显示 AI 思考过程"

    // ---------------- 云端账户 ----------------
    QString cloudEndpoint() const;              // 默认 https://your-worker.workers.dev
    bool    cloudEnabled() const;

    // ---------------- 界面 ----------------
    QString theme() const;                      // dark / light
    void    setTheme(const QString& t);
    QStringList enabledSources() const;
    void    setEnabledSources(const QStringList& ids);

    // ---------------- 设备 ----------------
    bool    autoConvertForDevice() const;       // 设备不支持 FLAC 时实时转 MP3
    int     deviceBitrate() const;              // 默认 320

    // ---------------- 搜索源凭据（可选） ----------------
    QString jamendoClientId() const;
    QString pixabayKey() const;

    // ---------------- 平台登录态 ----------------
    // 扫码登录成功后把 cookie 落盘。意义不只是"读私有歌单"：
    // 网易云拿音频直链（/api/song/enhance/player/url）时带上登录态能拿到
    // 更高码率、更多曲目，所以搜索和下载两处都要能读到它。
    // 以前 cookie 只活在 PlaylistParser 的一个成员变量里，重启就没了，
    // 下载器更是完全拿不到 —— 这也是"搜到却下不动"的一部分原因。
    QString neteaseCookie() const;
    void    setNeteaseCookie(const QString& cookie);
    QString qqCookie() const;
    void    setQQCookie(const QString& cookie);

    // ---------------- 外部工具 ----------------
    // 留空表示"让 YtDlpWrapper 自己按候选列表查找"，这是默认行为。
    // 只有当用户把 yt-dlp.exe 放在一个非常规位置（便携版 U 盘、
    // 多版本共存需要固定某个版本）时才需要显式指定。
    QString ytDlpPath() const;

    // settings.ini 的绝对路径。启动时打进日志，方便用户直接去改文件 ——
    // AppConfigLocation 在不同 Windows 版本 / 有无 setOrganizationName 的情况下
    // 展开结果不一样，与其在文档里写一个可能过期的路径，不如让程序自己报。
    QString filePath() const { return m_s.fileName(); }

private:
    Settings();
    mutable QSettings m_s;
};

} // namespace ms
