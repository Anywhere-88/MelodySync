// ============================================================================
//  src/ai/IntentParser.cpp
//
//  ★ 关键算法一：AI 匹配度评分
//  ---------------------------------------------------------------------------
//  总分 = 相关度 × 75  +  音质 × 10  +  免费 × 10  +  层次 × 5   （再做时长闸门）
//  其中"相关度"（relevanceScore，自身是 0-100 标尺）由三项按权重合成：
//
//      歌名相似度（多语言取最高）× 35
//      原唱一致度                × 25
//      热度（播放量/流行度）      × 15
//
//  为什么是这五项（用户明确指定的排序因子）：
//    - 歌名（多语言）35%：歌名不对一切白搭；且必须"多语言取最高" —— 用户用
//      中文译名搜日文歌时（"偶像"→アイドル），日文原唱与中文查询的相似度≈0，
//      不用 LLM 给的原语言别名兜底，原唱永远排不上来。
//    - 原唱 25%：搜歌最常见的坑是"搜到一堆翻唱/现场/加速版"。判据有三层，
//      从强到弱：① 平台标记（网易云 originCoverType，最可靠）② 歌手名一致
//      ③ 标题里的版本词（live/cover/remix/伴奏/加速/钢琴版…）。
//    - 热度 15%：同名翻唱满天飞时，播放量/流行度是区分"原唱或热门版本"和
//      "某个 UP 主随手传的"最有效的信号。国内热度来自网易云 pop 与哔哩哔哩
//      播放量，国外来自 YouTube 播放量 / SoundCloud 播放数。**拿不到数据时
//      取中性值**，绝不当成"0 播放"来惩罚 —— 否则 QQ 这种不返回播放量的源
//      会被单方面压死。
//    - 音质 10%：无损 > 320K > 128K。
//    - 免费 10%：能直接下载的排前面（对下载器来说"下得到"比"分更高"重要）。
//    - 层次 5%：平台直连源（网易云/QQ）小胜全网源 —— 元数据更准、更快，
//      这对应"先找网易云+QQ，再找全网"的检索顺序。
//
//  时长是**乘性闸门**而不是独立权重项：它只负责剔除"30 秒试听片段"和
//  "超长 live 剪辑"，不适合参与排序（用户不会因为时长更接近就更想要某一条）。
//      偏差 > 20% → ×0.75；> 35% → ×0.4；时长 < 45 秒（疑似试听片段）→ ×0.4
//  参照时长的取法见 scoreAll()：先关掉闸门算一遍相关度，挑出最像这首歌的、
//  **完整长度**的几条结果，取它们各自的时长组成"参照集合"，只要接近其中任意
//  一个就算长度对得上（录音室版/现场版/加长版长度本就不同）。两个坑都踩过：
//    · 取"结果集中位数" → 结果集以翻唱为主时，原唱被判成异类；
//    · 对着一个参照算 → 结果集以试听片段为主时，试听版自己当了参照，
//      1:29 被当成标准长度，5:35 的完整版反被判成"版本不对"。
//
//  歌名闸门（乘性）：歌名相似度 < 0.5 → ×0.6，< 0.3 → ×0.25。
//  歌名是用户唯一的强约束，它不成立时"原唱 + 热度"也不许把结果救上前排
//  （不然同一个歌手的另一首歌会靠歌手分混进来）。
//
//  歌手闸门（乘性）：查询给了歌手、候选也署了名，但两边明显不是同一人时，
//  歌名分 ×0.6。方向和歌名闸门相反 —— 那是"歌对不上就不算"，这是"人不对，
//  歌名对得上也不算"。必须有这道闸门：翻唱标题往往把原唱名字写在括号里
//  （「アイドル【Cover：YOASOBI】— 某UP主」），多语言歌名匹配会给满分，
//  光靠"原唱项扣分"压不住（实测翻唱靠名称 35 + 热度 11 跟原唱并列第一）。
//  而且现实里 Cover 拼成 Cvoer 这种词表根本抓不到，只能靠歌手字段判。
//
//  版本修正：用户明确要"现场版"而候选没有 live/现场 时，歌名分打折；
//            反之用户要"原版"而候选是 live/翻唱/remix/伴奏 时同样打折。
//            另外"试音/评测/合集/串烧/教程"这类与版本无关的坏内容一律重罚 ——
//            搜歌的人不会想要一个音箱测评视频。
//
//  同一个 relevanceScore 还被"换源下载"复用（DownloadEngine 用它从搜索候选里
//  挑出真正对应的那一首）。以前换源是直接抓搜索结果第一条，撞上翻唱/现场/
//  试音视频就下错了版本，用户反馈过好几次。
// ============================================================================
#include "ai/IntentParser.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace ms {

namespace {

// 自然语言特征：出现这些词说明用户在"描述"而不是"打字搜歌"
const char* kNaturalMarkers[] = {
    "那首", "开头", "一段", "找一下", "帮我找", "一首", "那种", "有点像",
    "背景", "配乐", "视频用", "抖音", "剪辑", "bmv", "bgm"
};

// 风格关键词 → 命中后影响 searchMode 与检索关键词
struct StyleRule { const char* pattern; const char* keyword; const char* mode; };
const StyleRule kStyleRules[] = {
    { "钢琴|piano|琴键",             "piano",      "background" },
    { "吉他|guitar|指弹",            "guitar",     "background" },
    { "爵士|jazz",                   "jazz",       "music" },
    { "安静|舒缓|放松|轻音乐|ambient", "ambient",   "background" },
    { "纯音乐|instrumental|伴奏",     "instrumental","background" },
    { "电子|edm|电音",               "electronic", "music" },
    { "摇滚|rock",                   "rock",       "music" },
    { "古典|classical|交响",          "classical",  "music" },
    { "音效|sfx|特效声",              "sfx",        "sfx" },
};

struct VersionRule { const char* pattern; const char* version; };
const VersionRule kVersionRules[] = {
    { "现场|live|演唱会",     "现场版" },
    { "翻唱|cover",           "翻唱" },
    { "remix|混音|重混",      "Remix" },
    { "伴奏|卡拉|instrumental", "伴奏" },
    { "钢琴版|piano version", "钢琴版" },
    { "加速|sped",            "加速版" },
    { "原版|原唱",            "原版" },
};

bool matchesAny(const QString& text, const QString& pattern)
{
    return QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption)
        .match(text).hasMatch();
}

// 去掉歌名里的"装饰性"后缀，便于做去重键
QString stripDecorations(const QString& raw)
{
    QString s = raw;
    s.remove(QRegularExpression(QStringLiteral(
        "\\s*[\\[（(【]\\s*(?:live|remix|cover|伴奏|现场|翻唱|feat\\.?|ft\\.?|"
        "official|mv|audio|hd|hq|无损|高音质|完整版|试听)[^\\]）)】]*[\\]）)】]\\s*"),
        QRegularExpression::CaseInsensitiveOption));
    return s.simplified();
}

// ---------------------------------------------------------------------------
// 版本 / 内容标记词表（评分用，与上面的 ruleBased() 共用一套口径）
// ---------------------------------------------------------------------------
// 与"版本"无关的坏内容：搜歌的人不会想要音箱测评、合集串烧或教学视频。
// 命中一律重罚，无论用户要的是哪个版本。
const char* kBadContentPattern =
    "试音|评测|测评|开箱|对比|盘点|排行|top\\s?\\d|合集|串烧|连唱|一小时|十小时|"
    "1\\s?hour|\\d+\\s?hours|loop|循环|教程|教学|tutorial|reaction|"
    "片段|预告|花絮|访谈|采访|解析|乐评|科普|"
    // 曲谱类：视频里放的是谱子或演奏示范，不是这首歌的音频。
    // （实测搜 The Other Side Of Paradise 时"架子鼓鼓谱""光遇琴谱"排得很靠前，
    //   而且它们都能被 yt-dlp 解析出音频 —— 不压的话是最典型的"下错版本"。）
    "琴谱|鼓谱|简谱|乐谱|扒谱|曲谱|吉他谱|尤克里里谱|数字谱|鼓点|打击垫|翻弹|"
    "bass ?tab|贝斯谱|谱面|tab谱|和弦谱|"
    // 器乐改编：打击乐/重奏这类"用别的编制演奏这首歌"的视频，
    // 实测走全网兜底时会靠标题里的原唱歌手名拿到很高的"原唱"分，
    // 不单独识别就会挤进前排。
    "打击乐|打楽器|重奏|四重奏|弦乐版|管乐|"
    // 舞蹈类：翻跳/编舞视频里的"音轨"只是舞者的伴奏，画面才是主体。
    // 实测搜 YOASOBI《アイドル》走全网兜底时，哔哩哔哩前排全是
    //   「全曲翻跳【桃桃】」「踊ってみた」「原创振付」
    // —— 它们播放量很高（几十万到上百万），热度项反而把它们顶到前面去。
    "翻跳|踊って|踊ってみた|振付|编舞|舞蹈|舞见|宅舞|"
    // 器材试听类：视频主体是"这首歌在这套设备上听起来怎么样"，
    // 而不是这首歌本身（常见于哔哩哔哩的 Hi-Fi 频道）。
    "录音棚|音箱|音响|耳机|车载|试听间|"
    // 学唱类：教唱/谐音/罗马音标的视频，主体是教学而不是这首歌。
    "学唱|教唱|跟唱|谐音|罗马音|空耳|音译";

// 原版之外的各种"版本"标记。用户没指定版本时要压它们；
// 用户明确要某个版本时只压"不匹配的那些"。
const char* kLivePattern    = "live|现场|演唱会|concert|unplugged|不插电";
const char* kCoverPattern   = "翻唱|cover|致敬|翻自|重新演绎|女声版|男声版|童声版|ai ?翻唱";
const char* kRemixPattern   = "remix|混音|dj版|bootleg|mashup|重混";
const char* kBackingPattern = "伴奏|instrumental|卡拉ok|karaoke|off ?vocal|纯音乐";
const char* kSpedPattern    = "加速|sped ?up|nightcore|变速|慢速|slowed|reverb";
const char* kAltVersionPattern = "钢琴版|吉他版|八音盒|music ?box|手风琴|小提琴版|尤克里里|指弹";

// 明确的"试听/片段"标记。QQ 音乐对 VIP 曲目会给出标了「试听版」的
// 1:29 短片段（128K），它是**能下载的**，所以"免费"这一项会给满分、
// 在只按"能不能下"判断的逻辑里非常靠前 —— 但用户要的根本不是它。
// 必须单独识别出来往下压（实测搜 YOASOBI《アイドル》时它抢到了第 1 名）。
// TV 版 / Anime Edit 也归在这里：它们是电视台 / 动画用的 1:29 短版本，
// 长度只有完整版的一半不到，用户搜歌要的不是它。
const char* kSnippetPattern =
    "试听|preview|片段|teaser|前 ?\\d+ ?秒|anime ?edit|tv ?size|tv ?ver|short ?ver|短版";

// 视频站的"标题党"包装词。这些词几乎只出现在二次剪辑 / 安利向投稿上，而不是
// 版权方发布的音轨，而且它们通常把歌名挤到标题的一小角 —— 配合"按占比给的
// 包含奖励"（见 Types.cpp 的 similarity）一起用效果最好。
// 惩罚比 kBadContentPattern 轻：命中说明"这大概率不是官方音轨"，不代表内容跑偏。
// 实测搜 Sugar 时前排是：
//   【4K60FPS】魔力红Maroon 5《Sugar》甜蜜神曲！祝大家情人节快乐
//   Sugar - Maroon 5【Hi-Res】百万级装备试听
// 两条都不是音轨，却靠"标题含歌名 0.92 + 歌手对得上"拿到 70 分以上。
const char* kPromoPattern =
    "神曲|私藏|珍藏|宝藏|安利|单曲循环|亿遍|循环播放|必听|"
    "4K|60FPS|1080P|8K|HDR|蓝光|无损音质|hi-?res|"
    "祝大家|一起来听|值得收藏|耳朵怀孕|吐血推荐";

// 名字是否作为"独立的词"出现在文本里。
// 纯 ASCII 的名字必须落在词边界上 —— 否则短名会大量误命中：
// "MAX" 命中 "Maximize"，"Sia" 命中 "Asia"，"MAX" 还会命中 "MAXIMUM"。
// 中文名没有词边界概念，直接 contains。
bool containsWord(const QString& haystack, const QString& needle)
{
    if (needle.isEmpty() || haystack.isEmpty()) return false;
    bool ascii = true;
    for (const QChar& c : needle) if (c.unicode() > 127) { ascii = false; break; }
    if (!ascii) return haystack.contains(needle, Qt::CaseInsensitive);

    const QRegularExpression re(
        QStringLiteral("(?<![A-Za-z0-9])") + QRegularExpression::escape(needle)
            + QStringLiteral("(?![A-Za-z0-9])"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(haystack).hasMatch();
}

// ⚠ 上面这些模式串里含中文，必须以 QString::fromUtf8 转成 QString 再用。
// 用 QLatin1String 会把 UTF-8 字节按 Latin-1 解读成乱码，中文分支直接失效
// （表现为"翻唱/live 的惩罚完全不起作用"，且不报任何错）。
inline QString utf8Pattern(const char* p) { return QString::fromUtf8(p); }

} // namespace

// ---------------------------------------------------------------------------
// 规则解析：离线兜底，保证没有 API Key 也能正常搜索
// ---------------------------------------------------------------------------
SearchIntent IntentParser::ruleBased(const QString& query)
{
    SearchIntent it;
    it.raw = query.trimmed();

    for (const char* m : kNaturalMarkers) {
        if (it.raw.contains(QString::fromUtf8(m), Qt::CaseInsensitive)) { it.isNatural = true; break; }
    }

    // 1) 优先取引号内的内容作为歌名（用户习惯用引号标出歌名/歌词片段）
    static const QRegularExpression quoted(
        QStringLiteral("[\"'“”‘’「」《》]([^\"'“”‘’「」《》]{1,40})[\"'“”‘’「」《》]"));
    const QRegularExpressionMatch qm = quoted.match(it.raw);
    if (qm.hasMatch()) {
        it.name = qm.captured(1).trimmed();
        it.keywords << it.name;
    }

    // 2) 没有引号：按 "歌手 歌名" 或 "歌手-歌名" 切分
    if (it.name.isEmpty()) {
        const QStringList parts = it.raw.split(
            QRegularExpression(QStringLiteral("[\\s\\-—–]+")), Qt::SkipEmptyParts);
        if (parts.size() >= 2 && parts.first().size() <= 12) {
            it.artist = parts.first();
            it.name   = parts.mid(1).join(QLatin1Char(' '));
        } else {
            it.name = it.raw;
        }
    }

    // 3) 形态：从"那首xxx开头的歌"这类描述里，把引号外的主干也并入关键词
    if (it.isNatural && it.name.size() < 2) it.name = it.raw;

    // 4) 版本识别
    for (const VersionRule& r : kVersionRules) {
        if (matchesAny(it.raw, QString::fromUtf8(r.pattern))) {
            it.version = QString::fromUtf8(r.version);
            break;
        }
    }
    if (it.version.isEmpty()) it.version = QStringLiteral("原版");

    // 5) 风格关键词 + 模式判定
    for (const StyleRule& r : kStyleRules) {
        if (matchesAny(it.raw, QString::fromUtf8(r.pattern))) {
            it.keywords << QString::fromUtf8(r.keyword);
            // sfx 优先级最高，其次是 background
            if (it.searchMode == QLatin1String("music") || r.mode[0] == 's')
                it.searchMode = QString::fromUtf8(r.mode);
        }
    }
    it.keywords.removeDuplicates();
    it.confidence = 0.55;          // 规则解析的置信度上限，LLM 结果通常 0.8+
    return it;
}

// ---------------------------------------------------------------------------
// LLM 解析
// ---------------------------------------------------------------------------
SearchIntent IntentParser::parseWithLlm(const QString& query, QString* errorOut)
{
    static const QString kSystem = QStringLiteral(R"(
你是一个音乐检索意图解析器。把用户的自然语言转换成严格的 JSON，不要输出任何解释。
JSON 字段：
  name        string  歌名（猜测出的最可能歌名；无法判断时填空字符串）
  artist      string  歌手名（无法判断填空字符串）
  album       string  专辑名（可空）
  version     string  版本，取值：原版/现场版/翻唱/Remix/伴奏/钢琴版/加速版
  keywords    array   检索关键词（风格、语言、情绪等，0-4 个，可以是英文以提升召回）
  searchMode  string  music | background | sfx
  isNatural   bool    是否是自然语言描述（而非"歌手 歌名"这种结构化输入）
  confidence  number  0-1 的置信度
  altQueries  array   原语言检索词（每个是完整的"歌手 歌名"词组）。
                      ★ 这是最重要的字段：用户十有八九用中文译名搜外语歌
                      （"偶像"→アイドル、"跨越时空"→粥ノ駆ける、"爱人"→Lover），
                      但平台曲库存的是原名，中文查询搜不到原唱。
                      只要你认出这首歌、且歌名原文不是中文，就**必须**给出：
                        例1 "YOASOBI 偶像"  → ["YOASOBI アイドル", "アイドル YOASOBI", "YOASOBI IDOL"]
                        例2 "周杰伦 晴天"   → []（原名就是中文）
                        例3 "泰勒 爱人"     → ["Taylor Swift Lover", "Lover Taylor Swift"]
                      拿不准就给出你最有把握的 1-2 个猜测，宁可多给不可漏给。
  altNames    array   原语言歌名（与 altQueries 对应，用于给结果打分）
  altArtist   string  原语言歌手名（可空；评分时与中文歌手名互补）
注意：
  - 如果用户只给了歌词片段（如"故事的小黄花"），把它当作 name，并在 keywords 里补上可能的歌名提示。
  - 如果用户描述的是"视频背景音乐"这类需求，searchMode 设为 background，keywords 用英文风格词。
  - name/artist 始终填用户输入语言的猜测值；altQueries/altNames/altArtist 才放原语言。
)");

    QString err;
    const QJsonObject o = m_ai.chatJson(kSystem, QStringLiteral("用户输入：") + query, &err, 0.1);
    if (o.isEmpty()) {
        if (errorOut) *errorOut = err;
        return SearchIntent();
    }

    SearchIntent it = SearchIntent::fromJson(o);
    it.raw = query;
    if (it.version.isEmpty()) it.version = QStringLiteral("原版");
    if (it.searchMode.isEmpty()) it.searchMode = QStringLiteral("music");
    // 原语言变体直接决定"译名搜歌"能不能找到原唱，必须可观测：
    // 模型不按格式回数组（如回成字符串/漏字段）时，这里立刻能看出来
    qInfo() << "[IntentParser] 原语言变体: queries =" << it.altQueries.size()
            << "names =" << it.altNames.size()
            << "artist =" << it.altArtist;
    return it;
}

SearchIntent IntentParser::parse(const QString& query, QString* usedEngine,
                                QString* llmErrorOut)
{
    const SearchIntent fallback = ruleBased(query);

    if (m_ai.ready()) {
        QString err;
        SearchIntent ai = parseWithLlm(query, &err);
        if (!ai.name.isEmpty() || !ai.artist.isEmpty()) {
            // LLM 结果与规则结果互补：LLM 漏掉的字段用规则补齐
            if (ai.keywords.isEmpty()) ai.keywords = fallback.keywords;
            if (ai.version.isEmpty())  ai.version  = fallback.version;
            if (usedEngine) *usedEngine = QStringLiteral("llm:") + m_ai.providerName();
            return ai;
        }
        qInfo() << "[IntentParser] LLM 解析失败，回落规则解析:" << err;
        // 把失败原因交出去：只在日志里说，用户在界面上只看到"没走 AI"，
        // 完全无法判断是 Key 过期、地址填错还是模型名不对。
        if (llmErrorOut) *llmErrorOut = err;
    }

    if (usedEngine) *usedEngine = QStringLiteral("rule");
    return fallback;
}

// ---------------------------------------------------------------------------
// 版本标记检查（供换源下载判断"标题里的歌手名可不可信"）
// ---------------------------------------------------------------------------
bool IntentParser::hasVersionOrSnippetMark(const QString& text)
{
    const QString t = text.toLower();
    return matchesAny(t, utf8Pattern(kLivePattern))
        || matchesAny(t, utf8Pattern(kCoverPattern))
        || matchesAny(t, utf8Pattern(kRemixPattern))
        || matchesAny(t, utf8Pattern(kBackingPattern))
        || matchesAny(t, utf8Pattern(kSpedPattern))
        || matchesAny(t, utf8Pattern(kAltVersionPattern))
        || matchesAny(t, utf8Pattern(kSnippetPattern));
}

QString IntentParser::creditedArtist(const QString& title, const QString& uploader,
                                     const SearchIntent& intent)
{
    if (title.isEmpty()) return QString();
    // 标题写着"翻唱 / 现场 / 试听 / remix…"时，里面出现歌手名并不代表
    // 唱的人就是这位歌手（「翻唱 YOASOBI」「YOASOBI 原曲 cover by …」都是这种）。
    if (hasVersionOrSnippetMark(title)) return QString();

    // ★ 逐个歌手名去找，而不是拿整个 artist 字段去比。
    // 歌单里的 artist 常是 "Cartoon / Jéja / Coleman Trapp" 这种拼接串，而站点标题
    // 写的是 "Cartoon,Jéja,Coleman Trapp"（甚至只写主唱 "Cartoon"）—— 整串 contains
    // 永远不成立，结果就是"署名认不出来 → 拿 uploader（往往是搬运号）当歌手 →
    // 原唱项归零 → 歌手闸门再 ×0.6"，正确的候选被自己人压死。
    QStringList names = splitArtists(intent.artist);
    for (const QString& extra : splitArtists(intent.altArtist)) names << extra;

    for (const QString& n : names) {
        const QString cand = n.trimmed();
        if (cand.size() < 2) continue;                     // 太短的名字容易误命中
        if (!containsWord(title, cand)) continue;
        // uploader 自己就对得上 → 本来就认得出来，不必改
        if (containsWord(uploader, cand)) return QString();
        return cand;
    }
    return QString();
}

// ---------------------------------------------------------------------------
// ★ 相关度（0-100 自身标尺）：歌名(多语言) 35 + 原唱 25 + 热度 15，再乘时长闸门
// ---------------------------------------------------------------------------
// 抽成独立函数是为了让"换源下载"能复用它 —— 换源时要在候选里挑出真正对应的
// 那一首，判据必须和搜索结果排序完全一致，否则用户会看到"搜出来是这个，
// 下下来是那个"。
double IntentParser::relevanceScore(const QString& title, const QString& artist,
                                    const QString& album, int durationSec,
                                    int coverFlag, double heatEffective,
                                    const SearchIntent& intent,
                                    int expectedDurationSec, QString* detailOut,
                                    const QVector<int>* altDurations)
{
    // ---- 1) 歌名相似度（多语言取最高） ----
    const QString queryName = intent.name.isEmpty() ? intent.raw : intent.name;
    double nameSim = similarity(queryName, title);
    for (const QString& alt : intent.altNames)
        nameSim = std::max(nameSim, similarity(alt, title));

    const QString text = (title + QLatin1Char(' ') + album).toLower();

    // ---- 2) 原唱一致度 ----
    // 两边都可能是多歌手串（查询来自歌单："MAX / Chromeo"；候选是平台/站点给的
    // "MAX"、"Chromeo" 或 "MAX,Chromeo"），所以按"所有配对取最高"来算 ——
    // 只要有一个名字对得上，就算这位歌手在这首歌里有署名。
    // 拿整串做一次比较只会得到"两边都不像"的结论（splitArtists 见 Types.cpp）。
    QStringList wantArtists = splitArtists(intent.artist);
    for (const QString& extra : splitArtists(intent.altArtist)) wantArtists << extra;
    const QStringList candArtists = splitArtists(artist);

    auto pairMatch = [](const QString& a0, const QString& b0) {
        const QString a = a0.toLower().trimmed();
        const QString b = b0.toLower().trimmed();
        if (a.isEmpty() || b.isEmpty()) return -1.0;
        if (a == b) return 1.0;
        if (a.contains(b) || b.contains(a)) return 0.8;
        const double sim = similarity(a, b);
        // "Maroon 5" vs "Maroon5"、"Jéja" vs "Jeja"：去掉标点与重音差异后就是
        // 同一个名字，Dice 会给到 0.95+，按 0.85 算而不是减 0.35 变成 0.6。
        if (sim >= 0.95) return 0.85;
        return std::max(0.0, sim - 0.35);                // 相似但不同人，明显压低
    };
    double origScore;
    if (wantArtists.isEmpty()) {
        origScore = 0.75;                                // 查询没给歌手 → 中性值
    } else if (candArtists.isEmpty()) {
        origScore = 0.3;                                 // 候选没署名
    } else {
        origScore = -1.0;
        for (const QString& a : wantArtists)
            for (const QString& b : candArtists)
                origScore = std::max(origScore, pairMatch(a, b));
        if (origScore < 0.0) origScore = 0.3;            // 互相都没匹配上
    }

    // ---- 3) 版本修正（同时作用于歌名分与原唱分） ----
    const QString wantVersion = intent.version.isEmpty() ? QStringLiteral("原版") : intent.version;
    const bool wantOriginal = wantVersion == QStringLiteral("原版");
    const bool hitLive    = matchesAny(text, utf8Pattern(kLivePattern));
    const bool hitCover   = matchesAny(text, utf8Pattern(kCoverPattern));
    const bool hitRemix   = matchesAny(text, utf8Pattern(kRemixPattern));
    const bool hitBacking = matchesAny(text, utf8Pattern(kBackingPattern));
    const bool hitSped    = matchesAny(text, utf8Pattern(kSpedPattern));
    const bool hitAltVer  = matchesAny(text, utf8Pattern(kAltVersionPattern));
    const bool hitVersion = hitLive || hitCover || hitRemix || hitBacking || hitSped || hitAltVer;

    if (!wantOriginal) {
        // 用户要的就是某个特定版本：只有"对上了"才算好，其它一律打折
        const bool matches =
            (wantVersion.contains(QStringLiteral("现场")) && hitLive) ||
            (wantVersion.contains(QStringLiteral("翻唱")) && hitCover) ||
            (wantVersion.startsWith(QStringLiteral("Remix"), Qt::CaseInsensitive) && hitRemix) ||
            (wantVersion.contains(QStringLiteral("伴奏")) && hitBacking) ||
            (wantVersion.contains(QStringLiteral("加速")) && hitSped) ||
            (wantVersion.contains(QStringLiteral("钢琴")) && hitAltVer);
        if (!matches) {
            nameSim  *= hitVersion ? 0.6 : 0.85;
            origScore *= 0.8;
        }
    } else if (hitVersion) {
        // 用户要原版，候选却是 live/翻唱/remix/伴奏/加速/钢琴版 —— 压到后面去。
        // （注释里一直写着这条，代码此前漏实现，导致"要原版"时翻唱照样前排。）
        // 惩罚分档："换了个人唱" > "改过节奏" > "同一人的现场/另一编曲"。
        // 现场版由原唱本人演绎，仍算"原唱"，所以它是最轻的一档 —— 只是
        // 录音室版本更该排前面。
        nameSim  *= 0.8;
        origScore *= 0.75;
        if (hitCover) origScore *= 0.80;     // 翻唱：换了人唱，最重的硬伤
        if (hitRemix) origScore *= 0.85;     // 混音：编曲被改过
        if (hitSped)  origScore *= 0.75;     // 加速/慢速：整首被变速
        if (hitLive)  origScore *= 0.85;     // 现场：原唱本人，但非录音室
    }

    // 明确的"试听/片段"标记：QQ 对 VIP 曲给出的 1:29 试听版就属此类。
    // 它**能下载**（所以"免费"项满分），但拿到的只是一段，不是用户要的歌。
    // 实测搜 YOASOBI《アイドル》时它靠"名称满 + 歌手满 + 能下"抢到第 1 名，
    // 把真正的完整版挤到后面。这里按"换了版本"从重处理。
    if (matchesAny(text, utf8Pattern(kSnippetPattern))) {
        nameSim  *= 0.70;
        origScore *= 0.55;
    }

    // 平台原生标记（比标题关键词可靠得多）：网易云 originCoverType
    if (coverFlag == 2)      origScore *= 0.35;                       // 平台确认是翻唱
    else if (coverFlag == 1) origScore = std::max(origScore, 0.9);    // 平台确认是原唱

    // 与版本无关的坏内容（试音/评测/合集/教程）—— 搜歌的人不会想要，重罚
    const bool badContent = matchesAny(text, utf8Pattern(kBadContentPattern));
    if (badContent) { nameSim *= 0.35; origScore *= 0.5; }

    // 视频站的"标题党"包装（神曲/私藏/4K60FPS/Hi-Res…）—— 这类投稿把歌名挤到
    // 标题的一小角，通常是二次剪辑而不是音轨。惩罚比上面轻一档：它说明"大概率
    // 不是官方音轨"，而不是"内容跑偏"。
    const bool promo = matchesAny(text, utf8Pattern(kPromoPattern));
    if (promo) { nameSim *= 0.6; origScore *= 0.8; }

    // ---- 4) 热度 ----
    // heatEffective < 0 表示所有源都没给热度信号 → 中性 0.5。
    // 有数据时映射到 [0.15, 1.0]：保底 0.15，避免"播放量 0"直接归零（新歌/冷门
    // 曲目本身没有错，只是不热门）。
    const double heatScore = heatEffective < 0.0
                                 ? 0.5
                                 : 0.15 + 0.85 * qBound(0.0, heatEffective, 1.0);

    // ---- 5) 时长闸门（乘性，不占权重） ----
    double durFactor = 1.0;
    if (durationSec > 0) {
        // 参照时长可能有多个：同一首歌的录音室版 / 现场版 / 加长版长度本就不同，
        // 只拿一个参照算，会在"结果集里同时存在多个合法版本"时误伤 —— 例如
        // 录音室 3:20 对着现场 5:35 的参照，偏差 40%，被判成"版本不对"压下去。
        // 所以只要接近**任意一个**参照就算长度对得上，取其中最小的偏差。
        auto ratioTo = [durationSec](int ref) {
            return std::abs(durationSec - ref) / static_cast<double>(ref);
        };
        double bestRatio = -1.0;
        if (expectedDurationSec > 0) bestRatio = ratioTo(expectedDurationSec);
        if (altDurations) {
            for (int ref : *altDurations) {
                if (ref <= 0) continue;
                bestRatio = (bestRatio < 0.0) ? ratioTo(ref)
                                              : std::min(bestRatio, ratioTo(ref));
            }
        }
        if (bestRatio >= 0.0) {
            if (bestRatio > 0.35)      durFactor = 0.4;
            else if (bestRatio > 0.20) durFactor = 0.75;
        }
        // 这一条不受参照影响：45 秒以下基本就是试听片段/短剪辑。
        // （原来写在"有参照时长"的分支里，没有参照时反而漏罚。）
        if (durationSec < 45) durFactor *= 0.4;
    }

    // ---- 6) 歌手闸门（乘性）：歌手对不上时，歌名分也不可信 ----
    // 用户查了歌手，候选歌名吻合却换了个歌手演唱 —— 这不是"同一首歌的正规版本"，
    // 而是翻唱/改编。实测搜 YOASOBI《アイドル》时，网易云前排一堆
    //   「偶像(アイドル)【Cvoer：YOASOBI】— 兰音Reine」/「偶像 — Nova新星」
    // 靠"歌名多语言满分 35 + 热度 11"拿到 69 分，跟原唱本人的现场版并列第一。
    // 注意括号里连 Cover 都拼错了（Cvoer），版本词表根本抓不到 —— 光靠词表
    // 堵不住，必须用"歌手对不对"这个更本质的判据。
    // 只在①查询给了歌手 ②候选署了名 ③歌手明显不匹配 时才打折，
    // 免得把"没署名的结果"和"用户没查歌手"两种情况一起误伤。
    const bool queryGaveArtist = !intent.artist.isEmpty() || !intent.altArtist.isEmpty();
    double artistGate = 1.0;
    if (queryGaveArtist && !artist.trimmed().isEmpty() && origScore < 0.5)
        artistGate = 0.60;
    nameSim *= artistGate;

    // ---- 7) 歌名闸门（乘性）：歌名对不上，其它项不许救场 ----
    // 没有这道闸门时，同一个歌手的**另一首歌**能靠"原唱 25 + 热度 15"混进前排
    // （实测搜 The Other Side Of Paradise 时，Glass Animals 的《Agnes》得了
    //   52 分排第 6 —— 歌名相似度是 0）。歌名是用户唯一的强约束，
    // 它不成立的时候，整条结果就不该有任何竞争力。
    const double nameGate = nameSim >= 0.50 ? 1.0
                          : nameSim >= 0.30 ? 0.6
                                            : 0.25;

    const double raw = (nameSim * 35.0 + origScore * 25.0 + heatScore * 15.0)
                       * durFactor * nameGate;
    const double rel = qBound(0.0, raw / 75.0 * 100.0, 100.0);

    if (detailOut) {
        *detailOut = QStringLiteral("名称%1 原唱%2 热度%3")
                         .arg(std::lround(nameSim * 35.0))
                         .arg(std::lround(origScore * 25.0))
                         .arg(std::lround(heatScore * 15.0));
        if (artistGate < 1.0)
            *detailOut += QStringLiteral(" ×歌手闸门%1").arg(artistGate, 0, 'f', 2);
        if (durFactor < 1.0)
            *detailOut += QStringLiteral(" ×时长%1").arg(durFactor, 0, 'f', 2);
        if (nameGate < 1.0)
            *detailOut += QStringLiteral(" ×歌名闸门%1").arg(nameGate, 0, 'f', 2);
    }
    return rel;
}

// ---------------------------------------------------------------------------
// ★ 评分（总分 0-100，公式见文件头）
// ---------------------------------------------------------------------------
double IntentParser::scoreOne(const SearchHit& hit,
                              const SearchIntent& intent,
                              int expectedDurationSec,
                              const SourceMeta& meta,
                              QString* detailOut,
                              double heatBoost,
                              const QVector<int>* altDurations)
{
    // 相关度（0-100 自身标尺）→ 折到 75 分
    const double heatEff = heatBoost > 0.0 ? qMin(1.0, std::max(hit.heat, 0.0) + heatBoost)
                                           : hit.heat;
    QString relDetail;
    const double rel = relevanceScore(hit.title, hit.artist, hit.album, hit.durationSec,
                                      hit.coverFlag, heatEff, intent,
                                      expectedDurationSec, &relDetail, altDurations);

    // ---- 音质（10 分）----
    double qualityScore;
    switch (hit.quality) {
    case Quality::HiRes:    qualityScore = 1.0;  break;
    case Quality::Lossless: qualityScore = 0.95; break;
    case Quality::K320:     qualityScore = 0.8;  break;
    case Quality::K192:     qualityScore = 0.6;  break;
    case Quality::K128:     qualityScore = 0.45; break;
    default:                qualityScore = 0.3;  break;    // 未知：中性偏低
    }

    // ---- 免费（10 分）----
    // 有直链 = 满；标了 free 但拿不到地址 = 0.6（还能试听）；
    // 平台明确只能试听 = 0.1（用户下载不到，必须排到最后）
    double freeScore = 1.0;
    if (!hit.downloadUrl.isEmpty())      freeScore = 1.0;
    else if (hit.previewOnly)            freeScore = 0.1;
    else if (hit.free)                   freeScore = 0.6;
    else                                 freeScore = 0.2;

    // ---- 层次（5 分）：平台直连源 > 全网源 ----
    const double tierScore = meta.tier == 0 ? 1.0 : 0.4;

    const double total = rel * 0.75 + qualityScore * 10.0 + freeScore * 10.0 + tierScore * 5.0;

    if (detailOut) {
        *detailOut = QStringLiteral("%1 音质%2 免费%3")
                         .arg(relDetail)
                         .arg(std::lround(qualityScore * 10.0))
                         .arg(std::lround(freeScore * 10.0));
    }
    return qBound(0.0, total, 100.0);
}

void IntentParser::scoreAll(QVector<SearchHit>& hits,
                            const SearchIntent& intent,
                            int expectedDurationSec,
                            const QHash<QString, SourceMeta>& sourceMeta)
{
    // 时长闸门要有一个"共识时长"作为参照。取法很关键 ——
    //
    // ❌ 以前用结果集的**中位时长**：它假设"多数结果是对的"。可搜歌时最典型的
    //    情况恰恰相反：结果集里塞满了翻唱/改编/加速版。实测搜
    //    The Other Side Of Paradise（原曲 5:20），网易云返回的 12 条里大半是
    //    2 分钟左右的翻唱，中位数被拉到 3:49 —— 于是真正的原唱被判成
    //    "时长偏差 40%"，闸门打到 0.4，从第 1 名掉到第 3 名，而 2 分钟的翻唱
    //    因为"接近中位数"拿了满分。用户看到的就是"搜出来的第一名不对"。
    //
    // ✅ 现在先**关掉时长闸门**算一遍相关度，挑出最像这首歌的几条结果当参照。
    //    逻辑是"谁最像这首歌，就以谁的长度为准"：名称、原唱、热度三项都不看
    //    时长的，正好是最可靠的判据。
    //
    // ⚠ 光这样还不够 —— 参照必须排除"短片段"，否则会把整批结果带偏。实测搜
    //    YOASOBI《アイドル》：QQ 返回的 1:29 试听版名字/歌手全中、又"能下载"，
    //    相关度最高被选成参照，于是真正的完整版（网易云的 5:35 现场）反被判成
    //    "时长偏差 276%"打到 0.4，掉到第 4 名，用户看到的头条就是一段试听。
    //    所以参照候选必须是完整时长（≥90 秒）且标题没明说"试听/片段"。
    //
    // ⚠ 参照也不该只有一个：录音室版、现场版、加长版长度本来就不同。只留一个
    //    参照时，录音室 3:20 对着现场 5:35 的参照会算出 40% 偏差而被误判。
    //    因此最多保留 3 个"互不相同的长度"一起当参照。
    QVector<int> anchors;
    if (expectedDurationSec > 0) {
        anchors << expectedDurationSec;
    } else {
        struct AnchorCand { int dur; double rel; };
        QVector<AnchorCand> cands;
        for (const SearchHit& h : hits) {
            if (h.durationSec < 90) continue;                    // 试听片段不适合当参照
            const QString t = (h.title + QLatin1Char(' ') + h.album).toLower();
            if (matchesAny(t, utf8Pattern(kSnippetPattern))) continue;
            QString ignored;
            const double rel = relevanceScore(h.title, h.artist, h.album, h.durationSec,
                                              h.coverFlag, h.heat, intent,
                                              /*expectedDurationSec=*/0, &ignored);
            // 相关度太低说明它压根不是这首歌，没有资格定义"这首歌该多长"
            if (rel >= 60.0) cands.push_back(AnchorCand{ h.durationSec, rel });
        }
        std::sort(cands.begin(), cands.end(),
                  [](const AnchorCand& a, const AnchorCand& b) {
                      return a.rel != b.rel ? a.rel > b.rel : a.dur > b.dur;
                  });
        for (const AnchorCand& c : cands) {
            // 8 秒以内视作"同一个长度"，避免参照被同一版本的近似值占满
            bool dup = false;
            for (int a : anchors) {
                if (std::abs(a - c.dur) <= 8) { dup = true; break; }
            }
            if (dup) continue;
            anchors << c.dur;
            if (anchors.size() >= 3) break;
        }
    }

    const int reference = anchors.isEmpty() ? 0 : anchors.first();
    // 兜底：一条时长都没有（全是未知/全是片段）→ 参照为空，闸门自动失效
    QVector<int> altAnchors = anchors;
    if (!altAnchors.isEmpty()) altAnchors.removeFirst();

    for (SearchHit& h : hits) {
        SourceMeta meta = sourceMeta.value(h.sourceId);
        if (meta.id.isEmpty()) {
            // 源没注册元信息时给一个保守默认值，避免评分崩掉
            meta.id = h.sourceId;
            meta.tier = 1;
        }
        // 热度微调：平台返回顺序本身就是"热度/相关度"的近似，给源内前几名
        // 一点点奖励。15 分制的热度项里每级 0.8 分封顶 —— 恰好能翻转同分条目
        // （排序函数里 1 分以内视作平分），又绝无可能压过真实的质量差距。
        const double heatBoost = h.sourceRank >= 0
                                     ? std::max(0.0, 0.06 - 0.012 * h.sourceRank)
                                     : 0.0;
        h.score = scoreOne(h, intent, reference, meta, &h.scoreDetail, heatBoost,
                           altAnchors.isEmpty() ? nullptr : &altAnchors);
    }
}

// ---------------------------------------------------------------------------
// ★ 智能去重
// ---------------------------------------------------------------------------
QString IntentParser::dedupeKey(const QString& title, const QString& artist)
{
    const QString t = stripDecorations(title).toLower()
        .remove(QRegularExpression(QStringLiteral("[\\s\\-_.·、，,！!？?：:'\"“”‘’()（）]")));
    QString a = artist.toLower().simplified();
    // 多歌手（"A/B"、"A feat. B"）只取第一个，避免因为署名差异漏合并。
    // 分隔符口径必须与打分共用 splitArtists —— 两处各写一份正则时，会出现
    // "同一条结果既没被去重、打分又认不出歌手"的诡异行为。
    const QStringList names = splitArtists(artist);
    if (!names.isEmpty()) a = names.first().toLower().simplified();
    return t + QLatin1Char('|') + a;
}

void IntentParser::dedupe(QVector<SearchHit>& hits)
{
    QHash<QString, int> indexOf;        // key -> 在 out 中的下标
    QVector<SearchHit> out;
    out.reserve(hits.size());

    for (const SearchHit& h : hits) {
        const QString key = dedupeKey(h.title, h.artist);
        const auto it = indexOf.constFind(key);
        if (it == indexOf.constEnd()) {
            indexOf.insert(key, out.size());
            out.append(h);
            continue;
        }

        // 已存在同曲：按 "无损优先 > 匹配度高 > 免费" 的优先级决定保留谁
        SearchHit& kept = out[it.value()];
        const bool betterQuality = qualityRank(h.quality) > qualityRank(kept.quality);
        const bool sameQualityBetterScore =
            qualityRank(h.quality) == qualityRank(kept.quality) && h.score > kept.score + 0.5;
        const bool sameScoreButFree = std::abs(h.score - kept.score) <= 0.5 && h.free && !kept.free;
        // 「能下载」优先于「匹配度高几分」：用户搜歌的目的就是把它拿走。
        // 关键前提：去重键已经是"规范化歌名 + 歌手"，能走到这里说明是同一首歌，
        // 所以拿 8 分以内的小幅匹配差换一个真的可下载的源是稳赚的。
        // （以前没这条：一条 95 分的仅试听曲会顶掉 88 分的可下载曲，
        //   用户看到的结果就是"搜到了，但全是仅试听"。）
        const bool downloadableWins = kept.downloadUrl.isEmpty() && !h.downloadUrl.isEmpty()
                                      && std::abs(h.score - kept.score) < 8.0;

        if (betterQuality || sameQualityBetterScore || sameScoreButFree || downloadableWins) {
            QStringList alts = kept.altSources;
            alts << kept.sourceId;
            alts.removeDuplicates();
            SearchHit merged = h;
            for (const QString& a : alts) if (a != merged.sourceId) merged.altSources << a;
            merged.altSources.removeDuplicates();
            kept = merged;
        } else if (h.sourceId != kept.sourceId) {
            kept.altSources << h.sourceId;
            kept.altSources.removeDuplicates();
        }
    }
    hits = out;
}

// ---------------------------------------------------------------------------
// AI 摘要文案
// ---------------------------------------------------------------------------
QString IntentParser::buildSummary(const QVector<SearchHit>& hits, const SearchIntent& intent)
{
    if (hits.isEmpty())
        return QStringLiteral("没有找到匹配结果，建议放宽关键词，或在设置里启用更多源。");

    const SearchHit& best = hits.first();
    int freeCount = 0, losslessCount = 0;
    for (const SearchHit& h : hits) {
        if (h.free) ++freeCount;
        if (qualityRank(h.quality) >= qualityRank(Quality::Lossless)) ++losslessCount;
    }

    // "为什么是它"：把评分拆解讲成人话，用户能看懂才敢点下载
    QString reason;
    if (best.free && qualityRank(best.quality) >= qualityRank(Quality::Lossless))
        reason = QStringLiteral("该源为合法免费源且提供无损音质，可直接下载");
    else if (best.free)
        reason = QStringLiteral("该源为合法免费源，可直接下载");
    else
        reason = QStringLiteral("该条仅提供平台官方试听，不提供下载");

    // 原唱 / 热度是本次排序的两个新因子，摘要里点一句，用户才知道系统在看什么
    QString extra;
    if (best.coverFlag == 1)          extra += QStringLiteral("，平台标记为原唱");
    if (!best.heatText.isEmpty())     extra += QStringLiteral("，") + best.heatText;

    QString alt;
    if (!best.altSources.isEmpty())
        alt = QStringLiteral("，另有 %1 个源也命中此曲").arg(best.altSources.size());

    return QStringLiteral("共命中 %1 条结果（%2 条可下载 / %3 条无损）。"
                          "推荐 <b>%4</b> 的《%5》：匹配度 %6%%，%7%8%9。")
        .arg(hits.size())
        .arg(freeCount)
        .arg(losslessCount)
        .arg(best.sourceId, best.title)
        .arg(static_cast<int>(std::lround(best.score)))
        .arg(reason, extra, alt);
}

} // namespace ms
