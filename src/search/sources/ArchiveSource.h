// ============================================================================
//  src/search/sources/ArchiveSource.h
//  Internet Archive —— 公版录音（1920s-1970s 大量历史录音）与开源音频合集。
//  公开 API，无需 Key。合规性最好：标记为 Public Domain 的录音在美国属公有领域。
// ============================================================================
#pragma once

#include "search/SearchSource.h"

#include <QJsonObject>

namespace ms {

class ArchiveSource : public SearchSource
{
public:
    ArchiveSource() = default;

    SourceMeta meta() const override;
    QVector<SearchHit> search(const SearchContext& ctx) override;

private:
    struct AudioFile { QString name; qint64 sizeKb = 0; Quality quality = Quality::Unknown; };
    static QVector<AudioFile> pickAudioFiles(const QJsonObject& metadata, qint64* durationOut);
};

} // namespace ms
