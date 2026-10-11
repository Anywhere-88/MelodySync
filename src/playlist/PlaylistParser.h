// ============================================================================
//  src/playlist/PlaylistParser.h
//  外部歌单解析总入口：识别平台 → 分派到具体实现 → 统一返回 ParsedPlaylist。
//  支持：网易云 / QQ 音乐 / 酷狗 / 酷我。
// ============================================================================
#pragma once

#include "core/Types.h"
#include "playlist/NeteaseApi.h"
#include "playlist/QQMusicApi.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace ms {

class PlaylistParser : public QObject
{
    Q_OBJECT

public:
    explicit PlaylistParser(QObject* parent = nullptr);

    // 自动识别平台
    static QString detectPlatform(const QString& url);

    // 同步解析（在后台线程调用）
    bool parse(const QString& url, const QString& platformHint,
               ParsedPlaylist* out, QString* errorOut);

    // ---- 网易云扫码登录透传（前端需要渲染二维码） ----
    QJsonObject qrCreate(QString* errorOut);
    QJsonObject qrPoll(const QString& unikey, QString* errorOut);
    bool neteaseLoggedIn() const { return m_netease.loggedIn(); }

    // 把解析结果保存为「我的歌单」
    static qint64 saveToLibrary(qint64 userId, const ParsedPlaylist& pl);

private:
    bool parseKugou(const QString& url, ParsedPlaylist* out, QString* errorOut);
    bool parseKuwo(const QString& url, ParsedPlaylist* out, QString* errorOut);

    NeteaseApi m_netease;
    QQMusicApi  m_qq;
    QString     m_kuwoCookie;
    QString     m_kuwoCsrfToken;
};

} // namespace ms
