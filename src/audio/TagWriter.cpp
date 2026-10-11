// ============================================================================
//  src/audio/TagWriter.cpp
//
//  为什么用 TagLib 而不是自己写 ID3 块：
//    - ID3v2 的帧同步、UTF-16 BOM、unsynchronisation 都是坑；
//    - FLAC 的 VorbisComment 与 MP4 的 ilst atom 结构完全不同，
//      TagLib 已经把这些差异封装成一个 FileRef，换格式不用改调用方代码；
//    - FLAC 封面要写进 PICTURE 元数据块，手写极易损坏文件。
// ============================================================================
#include "audio/TagWriter.h"

#include "core/HttpClient.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>

#include <taglib/attachedpictureframe.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/id3v2tag.h>
#include <taglib/mp4file.h>
#include <taglib/mpegfile.h>
#include <taglib/tag.h>
#include <taglib/textidentificationframe.h>
#include <taglib/tstring.h>
#include <taglib/tstringlist.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/vorbisfile.h>
#include <taglib/xiphcomment.h>

#include <memory>

namespace ms {

namespace {

// TagLib 的 String 在 Windows 上默认 UTF-8（taglib 2.x 使用 std::u8string 语义），
// 统一走 toTString 做一次显式转换，避免中文乱码。
TagLib::String toTString(const QString& s)
{
    return TagLib::String(s.toUtf8().constData(), TagLib::String::UTF8);
}

QString fromTString(const TagLib::String& s)
{
    return QString::fromUtf8(s.to8Bit(true).c_str());
}

} // namespace

QString TagWriter::formatOf(const QString& filePath)
{
    const QString ext = QFileInfo(filePath).suffix().toLower();
    if (ext == QLatin1String("m4a") || ext == QLatin1String("mp4")) return QStringLiteral("m4a");
    return ext;
}

// ---------------------------------------------------------------------------
bool TagWriter::write(const QString& filePath, const Meta& meta,
                      const QByteArray& coverData, QString* errorOut)
{
    if (!QFileInfo::exists(filePath)) {
        if (errorOut) *errorOut = QStringLiteral("文件不存在");
        return false;
    }

    // TagLib 在 Windows 下需要本地编码路径，QFile::encodeName 处理带中文的路径
    TagLib::FileRef ref(QFile::encodeName(filePath).constData(), true,
                        TagLib::AudioProperties::Fast);
    if (ref.isNull() || !ref.file()) {
        if (errorOut) *errorOut = QStringLiteral("TagLib 无法打开该格式：") + formatOf(filePath);
        return false;
    }

    TagLib::Tag* tag = ref.tag();
    if (!tag) {
        if (errorOut) *errorOut = QStringLiteral("该格式不支持标签写入");
        return false;
    }

    // ---- 基础字段 ----
    if (!meta.title.isEmpty())       tag->setTitle(toTString(meta.title));
    if (!meta.artist.isEmpty())      tag->setArtist(toTString(meta.artist));
    if (!meta.album.isEmpty())       tag->setAlbum(toTString(meta.album));
    if (!meta.genre.isEmpty())       tag->setGenre(toTString(meta.genre));
    if (!meta.comment.isEmpty())     tag->setComment(toTString(meta.comment));
    if (meta.year > 0)               tag->setYear(static_cast<unsigned int>(meta.year));
    if (meta.trackNo > 0)            tag->setTrack(static_cast<unsigned int>(meta.trackNo));
    ref.save();

    // ---- Album Artist + 歌词：这两项标准 Tag 没有统一入口，按容器分别处理 ----
    const QString ext = QFileInfo(filePath).suffix().toLower();

    if (ext == QLatin1String("flac")) {
        TagLib::FLAC::File flac(QFile::encodeName(filePath).constData());
        if (flac.isValid()) {
            TagLib::Ogg::XiphComment* xiph = flac.xiphComment(true);
            if (xiph) {
                if (!meta.albumArtist.isEmpty())
                    xiph->addField("ALBUMARTIST", toTString(meta.albumArtist), true);
                if (!meta.lyrics.isEmpty())
                    xiph->addField("LYRICS", toTString(meta.lyrics), true);
            }
            // 嵌入封面：FLAC 需要构造 FLAC::Picture 并 addPicture
            if (!coverData.isEmpty()) {
                flac.removePictures();
                auto* pic = new TagLib::FLAC::Picture();
                pic->setMimeType(QFileInfo(filePath).suffix().toLower() == QLatin1String("flac")
                                     ? "image/jpeg" : "image/jpeg");
                pic->setType(TagLib::FLAC::Picture::FrontCover);
                pic->setDescription("Cover");
                pic->setData(TagLib::ByteVector(coverData.constData(), coverData.size()));
                flac.addPicture(pic);   // flac 接管所有权
            }
            flac.save();
        }
    } else if (ext == QLatin1String("mp3")) {
        TagLib::MPEG::File mp3(QFile::encodeName(filePath).constData());
        if (mp3.isValid() && mp3.ID3v2Tag(true)) {
            TagLib::ID3v2::Tag* id3 = mp3.ID3v2Tag();

            // TagLib 2.x 移除了 ID3v2::Tag::setFrameText()，必须自己构造 Frame
            // 再 addFrame()（addFrame 接管所有权）。
            //
            // 注意这两种帧的类型并不相同，不能一律用 TextIdentificationFrame：
            //   TPE2（专辑艺术家）—— 普通文本帧  → TextIdentificationFrame
            //   USLT（歌词）      —— 专用歌词帧  → UnsynchronizedLyricsFrame
            // 用错类型会写出结构非法的 ID3 帧，播放器读不出来。
            if (!meta.albumArtist.isEmpty()) {
                id3->removeFrames("TPE2");
                auto* f = new TagLib::ID3v2::TextIdentificationFrame(
                    "TPE2", TagLib::String::UTF8);
                f->setText(toTString(meta.albumArtist));
                id3->addFrame(f);
            }
            if (!meta.lyrics.isEmpty()) {
                id3->removeFrames("USLT");
                auto* f = new TagLib::ID3v2::UnsynchronizedLyricsFrame(TagLib::String::UTF8);
                f->setText(toTString(meta.lyrics));
                id3->addFrame(f);
            }

            if (!coverData.isEmpty()) {
                id3->removeFrames("APIC");
                auto* frame = new TagLib::ID3v2::AttachedPictureFrame();
                frame->setMimeType("image/jpeg");
                frame->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
                frame->setDescription("Cover");
                frame->setPicture(TagLib::ByteVector(coverData.constData(), coverData.size()));
                id3->addFrame(frame);
            }
            mp3.save();
        }
    } else if (ext == QLatin1String("m4a") || ext == QLatin1String("mp4")) {
        TagLib::MP4::File mp4(QFile::encodeName(filePath).constData());
        if (mp4.isValid() && mp4.tag()) {
            TagLib::MP4::Tag* t = mp4.tag();
            if (!meta.albumArtist.isEmpty())
                t->setItem("aART", TagLib::StringList(toTString(meta.albumArtist)));
            if (!meta.lyrics.isEmpty())
                t->setItem("\xa9lyr", TagLib::StringList(toTString(meta.lyrics)));
            if (!coverData.isEmpty()) {
                // MP4 封面必须包一层 MP4::CoverArt，type 与 data 分开存
                TagLib::MP4::CoverArt art(TagLib::MP4::CoverArt::JPEG,
                    TagLib::ByteVector(coverData.constData(), coverData.size()));
                TagLib::MP4::CoverArtList list;
                list.append(art);
                t->setItem("covr", list);
            }
            mp4.save();
        }
    } else if (ext == QLatin1String("ogg")) {
        TagLib::Ogg::Vorbis::File ogg(QFile::encodeName(filePath).constData());
        if (ogg.isValid() && ogg.tag()) {
            if (!meta.albumArtist.isEmpty())
                ogg.tag()->addField("ALBUMARTIST", toTString(meta.albumArtist), true);
            if (!meta.lyrics.isEmpty())
                ogg.tag()->addField("LYRICS", toTString(meta.lyrics), true);
            ogg.save();
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
bool TagWriter::read(const QString& filePath, Meta* out, QString* errorOut)
{
    if (!QFileInfo::exists(filePath)) {
        if (errorOut) *errorOut = QStringLiteral("文件不存在");
        return false;
    }
    TagLib::FileRef ref(QFile::encodeName(filePath).constData());
    if (ref.isNull() || !ref.tag()) {
        if (errorOut) *errorOut = QStringLiteral("无法读取标签");
        return false;
    }
    TagLib::Tag* t = ref.tag();
    out->title  = fromTString(t->title());
    out->artist = fromTString(t->artist());
    out->album  = fromTString(t->album());
    out->genre  = fromTString(t->genre());
    out->year   = static_cast<int>(t->year());
    out->trackNo = static_cast<int>(t->track());
    return true;
}

// ---------------------------------------------------------------------------
QByteArray TagWriter::downloadCover(const QString& url, int timeoutMs)
{
    if (url.isEmpty()) return QByteArray();
    const HttpClient::Response r = HttpClient::get(url, {}, timeoutMs);
    if (!r.ok || r.body.size() < 512) return QByteArray();
    // 只接受图片魔数，避免把 HTML 错误页当成封面写进文件
    const QByteArray& b = r.body;
    const bool isJpeg = b.startsWith("\xFF\xD8\xFF");
    const bool isPng  = b.startsWith("\x89PNG");
    if (!isJpeg && !isPng) return QByteArray();
    return b;
}

// ---------------------------------------------------------------------------
bool TagWriter::writeLyrics(const QString& filePath, const QString& lyrics,
                            bool alsoWriteLrcFile, QString* errorOut)
{
    Meta meta;
    meta.lyrics = lyrics;
    if (!write(filePath, meta, QByteArray(), errorOut)) return false;

    if (alsoWriteLrcFile && !lyrics.isEmpty()) {
        const QString lrcPath = QFileInfo(filePath).absolutePath() + QLatin1Char('/')
                                + QFileInfo(filePath).completeBaseName() + QStringLiteral(".lrc");
        QFile f(lrcPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(lyrics.toUtf8());
            f.close();
        } else {
            qInfo() << "[TagWriter] 歌词文件写入失败:" << lrcPath;
        }
    }
    return true;
}

} // namespace ms
