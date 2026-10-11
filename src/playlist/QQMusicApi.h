// ============================================================================
//  src/playlist/QQMusicApi.h
//  QQ 音乐歌单解析。
//
//  接口说明：使用 QQ 音乐网页版自身的歌单详情接口（fcg_ucc_getcdinfo_byids_cp），
//            `getcdinfo` 系列接口在未登录时也能读取**公开歌单**的曲目列表。
//  合规说明：只取元信息；不请求 vkey、不解析任何加密参数、不下载音频。
// ============================================================================
#pragma once

#include "core/Types.h"

#include <QJsonObject>
#include <QString>

namespace ms {

class QQMusicApi
{
public:
    QQMusicApi() = default;

    static bool    isQQUrl(const QString& url);
    static QString extractPlaylistId(const QString& url);

    bool parsePlaylist(const QString& url, ParsedPlaylist* out, QString* errorOut);

    // 从 song.file 结构推断可用音质（QQ 音乐用 size_* 字段表达各档位是否存在）
    static Quality inferQuality(const QJsonObject& song);

private:
    QJsonObject fetchPlaylistJson(const QString& dissId, QString* errorOut);

    QString m_cookie;
};

} // namespace ms
