// ============================================================================
//  src/search/sources/JamendoSource.h
//  Jamendo Music —— CC 授权音乐平台，官方开放 API，提供 mp3 试听与直链下载。
//  文档：https://developer.jamendo.com/v3.0
//  合规：Jamendo 上的曲目均以 CC 协议发布，下载需遵守对应授权（BY / BY-NC 等）。
// ============================================================================
#pragma once

#include "search/SearchSource.h"

namespace ms {

class JamendoSource : public SearchSource
{
public:
    explicit JamendoSource(const QString& clientId = QString());

    SourceMeta meta() const override;
    QVector<SearchHit> search(const SearchContext& ctx) override;
    bool available() const override { return !m_clientId.isEmpty(); }
    QString unavailableReason() const override
    {
        return QStringLiteral("未配置 Jamendo client_id（设置 → 搜索源）");
    }

private:
    QString m_clientId;
};

} // namespace ms
