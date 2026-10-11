// ============================================================================
//  src/ai/IntentParser.h
//  意图解析 + AI 匹配度评分 + 智能去重。
//
//  本文件是"AI 全网搜"的核心：
//    1) parse()      —— 自然语言 → 结构化意图（LLM 优先，规则兜底）
//    2) scoreAll()   —— 对每条候选打分，公式见 cpp 顶部的详细推导
//    3) dedupe()     —— 同一首歌跨源合并，优先保留无损/高可信源
// ============================================================================
#pragma once

#include "ai/AiClient.h"
#include "core/Types.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace ms {

class IntentParser
{
public:
    IntentParser() = default;

    // 纯规则解析：无 API Key 时也必须能工作（离线可用是硬要求）
    static SearchIntent ruleBased(const QString& query);

    // 优先走 LLM；失败/超时/无 Key 时自动回落到 ruleBased
    SearchIntent parse(const QString& query, QString* usedEngine = nullptr,
                       QString* llmErrorOut = nullptr);

    // -----------------------------------------------------------------------
    // 评分（公式详解见 IntentParser.cpp 文件头）
    //   总分 = 相关度×75 + 音质×10 + 免费×10 + 层次×5
    //   相关度 = 歌名(多语言)×35 + 原唱×25 + 热度×15，再乘时长闸门
    //   expectedDurationSec = 0 时，函数内部会用结果集里"最强的那条完整结果"
    //   的时长当共识时长（不是中位数 —— 中位数在结果集以翻唱/试听版为主时会带偏）
    // -----------------------------------------------------------------------
    static void scoreAll(QVector<SearchHit>& hits,
                         const SearchIntent& intent,
                         int expectedDurationSec,
                         const QHash<QString, SourceMeta>& sourceMeta);

    // heatBoost：源内排名换算的热度微调（scoreAll 内部用，外部一般不用传）
    // altDurations：其余"合法参照时长"，语义同 relevanceScore（scoreAll 内部用）
    static double scoreOne(const SearchHit& hit,
                           const SearchIntent& intent,
                           int expectedDurationSec,
                           const SourceMeta& meta,
                           QString* detailOut = nullptr,
                           double heatBoost = 0.0,
                           const QVector<int>* altDurations = nullptr);

    // 纯相关度评分（0-100 自身标尺）：只看"歌名（多语言）+ 原唱 + 热度 + 时长"，
    // 与"音质/免费/来源"无关。
    // **换源下载专用**：DownloadEngine 要在搜索候选里挑出真正对应的那一首，
    // 判据必须和搜索结果排序一致，否则会出现"搜出来是这首、下下来是那首"。
    //
    // altDurations：可选的"额外参照时长"。同一首歌的录音室版 / 现场版 / 加长版
    // 长度本就不同，只给一个参照会在结果集里同时存在多个合法版本时误伤
    // （实测：录音室 3:20 对着现场 5:35 的参照算，偏差 40% 被判成"版本不对"）。
    // 传入多个参照时，只要接近其中任意一个就算长度对得上。
    static double relevanceScore(const QString& title, const QString& artist,
                                 const QString& album, int durationSec,
                                 int coverFlag, double heatEffective,
                                 const SearchIntent& intent,
                                 int expectedDurationSec,
                                 QString* detailOut = nullptr,
                                 const QVector<int>* altDurations = nullptr);

    // -----------------------------------------------------------------------
    // 智能去重：同一首歌在多个源上可能各有一条结果，
    // 合并规则 = 主键(归一化 歌名+歌手) → 保留 qualityRank 最高者；
    //          qualityRank 相同时比 score；都比不出时优先 free 的源。
    // -----------------------------------------------------------------------
    static void dedupe(QVector<SearchHit>& hits);

    // 标题/专辑里是否带 live / 翻唱 / remix / 伴奏 / 加速 / 试听 之类的版本标记。
    // 供"换源下载"复用：视频站把歌手名写在标题里（「【字幕组】歌名 - YOASOBI」），
    // 这时歌手名能不能当"署名"看，得先确认标题没写着"翻唱/现场/试听"。
    static bool hasVersionOrSnippetMark(const QString& text);

    // -----------------------------------------------------------------------
    // 视频站候选的"署名"判定（全网源 + 换源下载两处共用）
    //
    // 视频站（哔哩哔哩/YouTube）的惯例是歌手名写在**标题**里，
    // 而 uploader / channel 是字幕组、搬运号、UP 主本人。
    // 直接拿 uploader 当歌手去比对，"原唱"这一项几乎恒为 0 —— 25 分白丢，
    // 结果就是"明明是对的版本却排不上来/过不了相关度门槛"。
    // 返回查询歌手名（标题里认出来的那位）表示可以用作署名；
    // 返回空串表示"没有可信署名，沿用 uploader"。
    static QString creditedArtist(const QString& title, const QString& uploader,
                                  const SearchIntent& intent);

    // 归一化键：小写、去括号内容、去空白与符号
    static QString dedupeKey(const QString& title, const QString& artist);

    // 由 LLM 生成"为什么推荐这条"的中文短句（失败时用本地模板）
    static QString buildSummary(const QVector<SearchHit>& hits, const SearchIntent& intent);

private:
    // 注意：这个函数不能是 static —— 它内部要用 m_ai（AiClient 成员）。
    // 之前误声明成 static 时，编译器对函数体里的 m_ai.chatJson(...) 报：
    //   error C2228: ".chatJson"的左边必须有类/结构/联合
    // 因为静态成员函数里访问非静态成员本身就是无效表达式。
    SearchIntent parseWithLlm(const QString& query, QString* errorOut);

    AiClient m_ai;
};

} // namespace ms
