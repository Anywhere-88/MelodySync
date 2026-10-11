// ============================================================================
//  src/search/sources/WebSource.h
//  「全网搜索」源 —— 把过去分散的哔哩哔哩 / YouTube Music / SoundCloud 三个
//  源合并成一个源，串行级联多个站点，对用户只呈现一个"全网"徽标。
//
//  为什么要合并（而不是继续并列三个源）：
//    1) yt-dlp 进程是**串行闸门**（PyInstaller 单文件包并发解包会互抢磁盘，
//       见 YtDlpWrapper 的 ProcGate），所以三个源名义上"并行"、实际排队，
//       等于三次进程启动开销（每次 3~5 秒），却只换来三个进度徽标。
//    2) 排队顺序全靠"线程谁先抢到空闸门"，很不稳定：不可达的 YouTube 一旦
//       抢到闸门会占住 30~44 秒，把国内唯一稳定的哔哩哔哩活活饿死到总超时。
//       合并之后顺序由代码写死（哔哩哔哩优先），不再靠抢。
//    3) 每个站点可以有自己的时间预算，慢站吃不完整个搜索的时间。
//
//  站点顺序与预算（见 .cpp 的 kSites）：
//    哔哩哔哩（国内可直连，必须最先跑，完整模式拿标题/时长/播放量）
//      → YouTube（flat，有代理的用户才有结果）
//      → SoundCloud（flat，同上）
//
//  不可达冷却：某站点硬失败（连不上/超时）后记入进程内冷却表，10 分钟内
//  不再试它。这条是"搜索总是卡在 YouTube Music"那个老问题的正面解法 ——
//  不可达的站点只让用户付一次代价（首次搜索多等几秒），之后不再拖累。
// ============================================================================
#pragma once

#include "search/SearchSource.h"

namespace ms {

class WebSource : public SearchSource
{
public:
    WebSource() = default;

    SourceMeta meta() const override;
    QVector<SearchHit> search(const SearchContext& ctx) override;

    // 依赖 yt-dlp.exe，它坏了这个源就整个废掉 —— 设置页要能显示出来
    bool    available() const override;
    QString unavailableReason() const override;
};

} // namespace ms
