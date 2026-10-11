// ============================================================================
//  src/search/sources/PreviewSource.h
//  平台源：网易云音乐 / QQ 音乐。
//
//  它是「第一层」：这两个源走平台自己的公开 HTTP 接口，秒级返回，元数据
//  （歌名/歌手/专辑/时长）准确，还能直接换到官方音频直链 —— 免费曲目当场
//  就能下载，付费曲目平台会如实拒绝，我们照实标"仅试听"并把原因写进
//  SearchHit::note，不重试、不变通、不绕过。
//
//  至于"这两个之外的东西"，全部交给「全网」源（WebSource，yt-dlp 驱动）。
//  历史上这里还挂着 YouTube Music / SoundCloud / 哔哩哔哩 三个 yt-dlp 源，
//  以及更早的 Jamendo / FMA / Internet Archive 三个 CC 源。它们的问题：
//    - CC 源要么已下线（Pixabay 没有音乐 API）、要么国内连不上（FMA/archive
//      8 秒超时），要么要申请 client_id 才有结果 —— 勾了等于没勾；
//    - YouTube/SoundCloud 国内必被墙，却和其它源抢同一把 yt-dlp 串行闸门，
//      一挂 30~44 秒，把哔哩哔哩活活饿死到总超时。
//  合并成一个「全网」源之后，站点顺序由代码写死、每个站点有独立预算，
//  不可达的站点还有冷却期，界面上的徽标也从 8 个减到 3 个。
//
//  不伪造签名、不绕过 DRM、不注入付费凭证；需要会员的曲目平台会明确拒绝。
// ============================================================================
#pragma once

#include "search/SearchSource.h"

namespace ms {

class PreviewSource : public SearchSource
{
public:
    explicit PreviewSource(const QString& platformId);

    SourceMeta meta() const override;
    QVector<SearchHit> search(const SearchContext& ctx) override;

    bool    available() const override;
    QString unavailableReason() const override;

    // 一次拿到全部平台源的元信息（SearchAggregator 注册时用）
    static QVector<SourceMeta> allMetas();

private:
    QVector<SearchHit> searchNetease(const SearchContext& ctx);
    QVector<SearchHit> searchQQMusic(const SearchContext& ctx);

    QString m_platform;      // netease / qq
};

} // namespace ms
