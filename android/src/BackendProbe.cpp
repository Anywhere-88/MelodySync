// ============================================================================
//  src/BackendProbe.cpp —— 骨架阶段自检项
//  1) 意图解析：ruleBased("Checklist MAX") → 歌名/歌手拆对
//  2) 相关度评分：正确候选应显著高于"同歌手另一首"（三道闸门起效）
//  3) 版本标记：promo/试听标题应被识别
//  4) 智能去重：同曲多源合并保留 qualityRank 最高者
//  全部调用与桌面版同一份源码，零改动交叉编译。
// ============================================================================
#include "BackendProbe.h"

#include "ai/IntentParser.h"
#include "core/Types.h"

namespace ms {

BackendProbe::BackendProbe(QObject* parent)
    : QObject(parent)
{
}

void BackendProbe::addRow(const QString& name, const QString& value, bool ok)
{
    QVariantMap row;
    row.insert(QStringLiteral("name"), name);
    row.insert(QStringLiteral("value"), value);
    row.insert(QStringLiteral("ok"), ok);
    m_rows.append(row);
    if (!ok)
        m_allOk = false;
}

void BackendProbe::run()
{
    m_rows.clear();
    m_allOk = true;

    // ---- 1. 规则意图解析 ----
    const SearchIntent intent = IntentParser::ruleBased(QStringLiteral("Checklist MAX"));
    addRow(QStringLiteral("意图解析·歌名"),
           intent.name.isEmpty() ? QStringLiteral("(空)") : intent.name,
           intent.name.contains(QStringLiteral("Checklist"), Qt::CaseInsensitive));
    addRow(QStringLiteral("意图解析·歌手"),
           intent.artist.isEmpty() ? QStringLiteral("(空)") : intent.artist,
           intent.artist.compare(QStringLiteral("MAX"), Qt::CaseInsensitive) == 0);

    // ---- 2. 相关度评分：正确候选 vs 同歌手另一首 ----
    //      合成候选字段与平台返回同构（title 带包装、durationSec 给参照）
    const int anchorSec = 135; // Checklist 官方 MV 时长
    struct Cand { const char* title; const char* artist; int dur; const char* label; };
    const Cand cands[] = {
        { "Checklist (feat. Chromeo) Official MV", "MAX",       anchorSec,      "正确候选" },
        { "MAX - Miss Can't Be Tamed",             "MAX",       anchorSec + 10, "同歌手另一首" },
        { "【中字】【BILLKIN】《CHECKLIST》Official MV", "BILLKIN", anchorSec,   "同名异曲" },
    };
    double scores[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; ++i) {
        scores[i] = IntentParser::relevanceScore(
            QString::fromUtf8(cands[i].title), QString::fromUtf8(cands[i].artist),
            QString(), cands[i].dur, 0, 0.5, intent, anchorSec);
    }
    addRow(QStringLiteral("评分·正确候选"),
           QString::number(scores[0], 'f', 1),
           scores[0] >= 60.0);
    addRow(QStringLiteral("评分·同歌手另一首"),
           QString::number(scores[1], 'f', 1),
           scores[1] < scores[0]);
    addRow(QStringLiteral("评分·同名异曲"),
           QString::number(scores[2], 'f', 1),
           scores[2] < scores[0]);

    // ---- 3. 版本/试听标记识别 ----
    addRow(QStringLiteral("版本标记·试听"),
           QStringLiteral("preview 命中"),
           IntentParser::hasVersionOrSnippetMark(QStringLiteral("Checklist preview 30s")));
    addRow(QStringLiteral("版本标记·正常标题"),
           QStringLiteral("Official MV 不误伤"),
           !IntentParser::hasVersionOrSnippetMark(QStringLiteral("Checklist Official MV")));

    // ---- 4. 智能去重 ----
    QVector<SearchHit> hits;
    SearchHit a; // 无损、低分
    a.id = QStringLiteral("web:1");
    a.sourceId = QStringLiteral("web");
    a.title = QStringLiteral("Checklist");
    a.artist = QStringLiteral("MAX");
    a.quality = Quality::Lossless;
    a.score = 70.0;
    SearchHit b; // 有损、高分
    b.id = QStringLiteral("web:2");
    b.sourceId = QStringLiteral("web2");
    b.title = QStringLiteral("Checklist");
    b.artist = QStringLiteral("MAX");
    b.quality = Quality::K128;
    b.score = 90.0;
    hits.append(a);
    hits.append(b);
    IntentParser::dedupe(hits);
    addRow(QStringLiteral("去重·合并后条数"),
           QString::number(hits.size()),
           hits.size() == 1);
    addRow(QStringLiteral("去重·保留高音质"),
           hits.isEmpty() ? QStringLiteral("(空)") : hits.first().id,
           !hits.isEmpty() && hits.first().id == QStringLiteral("web:1"));

    emit resultChanged();
}

} // namespace ms
