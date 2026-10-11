// ============================================================================
//  src/search/sources/FmaSource.h
//  Free Music Archive —— CC 授权独立音乐。
//
//  说明：FMA 官方 REST API（api.freemusicarchive.org）已于 2019 年停止服务，
//       官方推荐的做法是使用 Internet Archive 上的 fmacollection 镜像检索。
//       因此本实现走 archive.org 的 advancedsearch + metadata 两步查询，
//       拿到 identifier 后再解析具体的 mp3/flac 文件直链。
// ============================================================================
#pragma once

#include "search/SearchSource.h"

#include <QJsonObject>

namespace ms {

class FmaSource : public SearchSource
{
public:
    FmaSource() = default;

    SourceMeta meta() const override;
    QVector<SearchHit> search(const SearchContext& ctx) override;

private:
    // 从 archive.org 的 metadata 里挑出音频文件，返回 {文件名, 大小KB, 格式}
    struct AudioFile { QString name; qint64 sizeKb = 0; Quality quality = Quality::Unknown; };
    static QVector<AudioFile> pickAudioFiles(const QJsonObject& metadata);
};

} // namespace ms
