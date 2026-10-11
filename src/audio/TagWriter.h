// ============================================================================
//  src/audio/TagWriter.h
//  基于 TagLib 的元数据写入：歌名 / 歌手 / 专辑 / 年份 / 流派 / 音轨号 + 封面 + 歌词。
//  支持 FLAC / MP3(ID3v2) / M4A(MP4) / OGG / WAV，由 TagLib 自动分派。
// ============================================================================
#pragma once

#include "core/Types.h"

#include <QByteArray>
#include <QString>

namespace ms {

class TagWriter
{
public:
    struct Meta
    {
        QString title;
        QString artist;
        QString album;
        QString albumArtist;
        QString genre;
        QString comment;
        int     year = 0;
        int     trackNo = 0;
        QString lyrics;
    };

    // 写入标签。coverData 为空则跳过封面。
    // 返回 false 时 errorOut 给出原因（多数情况是文件格式不被 TagLib 支持）。
    static bool write(const QString& filePath,
                      const Meta& meta,
                      const QByteArray& coverData = QByteArray(),
                      QString* errorOut = nullptr);

    // 从文件读取标签（用于"音乐库重建"与同步前的校验）
    static bool read(const QString& filePath, Meta* out, QString* errorOut = nullptr);

    // 从网络地址拉取封面字节（失败返回空，不阻塞主流程）
    static QByteArray downloadCover(const QString& url, int timeoutMs = 10000);

    // 写歌词：优先内嵌（FLAC/MP3 支持），同时可选落一份同名 .lrc
    static bool writeLyrics(const QString& filePath, const QString& lyrics,
                            bool alsoWriteLrcFile, QString* errorOut = nullptr);

    // 依据文件后缀推断曲库展示用的"格式"字段
    static QString formatOf(const QString& filePath);
};

} // namespace ms
