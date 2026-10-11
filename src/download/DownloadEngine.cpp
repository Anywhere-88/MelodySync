// ============================================================================
//  src/download/DownloadEngine.cpp
// ============================================================================
#include "download/DownloadEngine.h"

#include "ai/IntentParser.h"
#include "audio/TagWriter.h"
#include "core/HttpClient.h"
#include "core/Types.h"
#include "data/Database.h"
#include "download/YtDlpWrapper.h"
#include "playlist/PlatformResolve.h"
#include "search/sources/WebSource.h"
#include "settings/Settings.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QThread>
#include <QUuid>

#include <algorithm>

namespace ms {

namespace {
QString newTaskId()
{
    return QStringLiteral("t") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

// ---------------------------------------------------------------------------
// ★ 换源下载：候选打分选优
// ---------------------------------------------------------------------------
// 以前的写法是 `bilisearch1:关键词` —— 让 yt-dlp 抓搜索结果**第一条**。
// 结果就是"货不对版本"：第一条经常是翻唱、现场、加速版、钢琴版，甚至音箱
// 试音/测评视频（用户反馈过好几次，例如搜 The Other Side Of Paradise）。
//
// 现在的做法和搜索结果排序完全共用一套判据（IntentParser::relevanceScore）：
//   1) 用 WebSource 跑**全网站点级联**搜一批候选（非 flat 模式，每条都带
//      标题/作者/时长/播放量）；
//   2) 逐条算相关度（歌名多语言 + 原唱 + 热度 + 时长闸门）；
//   3) 按相关度降序返回，下载时从最好的开始试。
// 这样"下下来的是什么"就由分数说了算，而不是由站点的排序运气说了算。
//
// 候选池为什么必须是"全网"而不是只看哔哩哔哩：见 rankWebCandidates 里的说明。
struct WebPick
{
    QString pageUrl;
    QString title;
    QString artist;
    double  rel = 0.0;      // 0-100
    QString detail;
};

// 低于这个相关度就宁可不下载 —— 与其给用户一个错的版本，
// 不如明确告诉他"没找到能确认对应的版本"。60 分大致相当于
// "歌名基本对上但歌手对不上"的水平。
constexpr double kMinFallbackRel = 60.0;

QVector<WebPick> rankWebCandidates(const DownloadTask& task, const CancelTokenPtr& cancel)
{
    QVector<WebPick> picks;
    YtDlpWrapper yt;
    if (!yt.available() || !yt.healthy()) return picks;

    // 打分意图：歌名与歌手**分开**放 —— relevanceScore 对两者分别算相似度，
    // 揉成一个字符串去比标题会让"歌手+歌名"整体匹配不上（标题里两者往往不相邻）。
    SearchIntent intent;
    intent.name    = task.title;
    intent.artist  = task.artist;
    intent.version = QStringLiteral("原版");

    // ★ 候选池走**全网站点级联**（哔哩哔哩 / YouTube / SoundCloud），而不是只搜
    //   哔哩哔哩。以前这里写死 bilisearch，等于把最好的候选直接排除在外：
    //     · 视频站上的"音轨"多是二次压制与搬运，翻唱、伴奏、合集、试听评测泛滥；
    //     · YouTube 有版权方自己的官方音轨（Topic 频道），SoundCloud 上独立音乐人
    //       常自己发布原曲 —— 有代理的用户本来能拿到干净得多的版本。
    //   直接复用 WebSource::search()：站点顺序、连通性预检、不可达冷却、署名判定、
    //   热度换算它都已经做好，换源与搜索排序用的是同一套口径 —— 用户看到
    //   "搜出来是这个、下下来是那个"，根子就是这两处口径不一致。
    qInfo() << "[Download] 换源意图: 歌名=" << intent.name << "| 歌手=" << intent.artist
            << "| 站点检索词=" << intent.siteSearchText()
            << "| 目标时长=" << task.durationSec << "s";
    SearchContext ctx;
    ctx.intent   = intent;
    ctx.cancel   = cancel;
    ctx.budgetMs = 25000;      // 换源是用户主动点的、可等待的操作（搜索另有 20s 总超时）
    WebSource web;
    const QVector<SearchHit> hits = web.search(ctx);
    if (hits.isEmpty()) {
        qInfo() << "[Download] 换源候选：全网站点级联没有返回任何结果";
        return picks;
    }

    for (const SearchHit& h : hits) {
        if (h.title.isEmpty() || h.downloadUrl.isEmpty()) continue;
        WebPick p;
        p.pageUrl = h.downloadUrl;          // web 源给的是页面地址，下载时再由 yt-dlp 解析
        p.title   = h.title;
        p.artist  = h.artist;               // WebSource 已经做过署名判定
        p.rel = IntentParser::relevanceScore(
            h.title, h.artist, h.album, h.durationSec, h.coverFlag,
            h.heat, intent, task.durationSec, &p.detail);
        picks.append(p);
    }

    std::sort(picks.begin(), picks.end(),
              [](const WebPick& a, const WebPick& b) { return a.rel > b.rel; });

    // 打印前 5 条（含评分拆解）：调门槛/权重时全靠它看清每一项的贡献
    for (int i = 0; i < picks.size() && i < 5; ++i)
        qInfo() << "[Download] 换源候选" << (i + 1) << ":" << picks[i].title
                << "|" << picks[i].artist << "| 相关度" << static_cast<int>(picks[i].rel)
                << "|" << picks[i].detail;
    return picks;
}

} // namespace

DownloadEngine::DownloadEngine(QObject* parent) : QObject(parent)
{
    m_concurrency = Settings::instance().concurrent();
}

DownloadEngine::~DownloadEngine()
{
    // 退出时把还在跑的取消掉，避免进程退出时线程仍在写文件
    QMutexLocker lock(&m_mutex);
    for (const CancelTokenPtr& c : m_cancels) if (c) c->cancel();
}

// ---------------------------------------------------------------------------
void DownloadEngine::setConcurrency(int n)
{
    QMutexLocker lock(&m_mutex);
    m_concurrency = qBound(1, n, 8);
    Settings::instance().setConcurrent(m_concurrency);
    lock.unlock();
    pump();
}

int DownloadEngine::concurrency() const
{
    QMutexLocker lock(&m_mutex);
    return m_concurrency;
}

QVector<DownloadTask> DownloadEngine::tasks() const
{
    QMutexLocker lock(&m_mutex);
    return m_tasks;
}

QString DownloadEngine::queueJson() const
{
    QMutexLocker lock(&m_mutex);
    QJsonArray arr;
    for (const DownloadTask& t : m_tasks) arr.append(t.toJson());
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

bool DownloadEngine::isDuplicate(const DownloadTask& t) const
{
    for (const DownloadTask& e : m_tasks) {
        if (e.title == t.title && e.artist == t.artist && e.quality == t.quality
            && e.status != TaskStatus::Done && e.status != TaskStatus::Canceled)
            return true;
    }
    return false;
}

QString DownloadEngine::buildSavePath(const DownloadTask& task, const QString& ext) const
{
    const QString dir = Settings::instance().downloadDir();
    QDir().mkpath(dir);
    // 文件名规则："歌手 - 歌名.扩展名"，与大多数音乐播放器的库扫描习惯一致
    const QString base = task.artist.isEmpty()
                             ? task.title
                             : (task.artist + QStringLiteral(" - ") + task.title);
    return QDir(dir).filePath(sanitizeFileName(base) + QLatin1Char('.') + ext);
}

// ---------------------------------------------------------------------------
int DownloadEngine::enqueue(const QVector<DownloadTask>& incoming)
{
    int added = 0;
    {
        QMutexLocker lock(&m_mutex);
        for (DownloadTask t : incoming) {
            if (t.id.isEmpty()) t.id = newTaskId();
            if (t.title.isEmpty()) continue;
            if (isDuplicate(t)) continue;
            t.status = TaskStatus::Pending;
            t.percent = 0;
            t.speedBps = 0;
            t.error.clear();
            m_tasks.append(t);
            m_cancels.append(std::make_shared<CancelToken>());
            m_running.append(false);
            ++added;
        }
    }
    if (added > 0) {
        emitQueue();
        emit toast(QStringLiteral("已加入 %1 首到下载队列").arg(added), QString());
        pump();
    }
    return added;
}

void DownloadEngine::startAll()
{
    {
        QMutexLocker lock(&m_mutex);
        for (int i = 0; i < m_tasks.size(); ++i) {
            if (m_tasks[i].status == TaskStatus::Paused
                || m_tasks[i].status == TaskStatus::Failed) {
                m_tasks[i].status = TaskStatus::Pending;
                m_tasks[i].error.clear();
                if (m_cancels[i]) m_cancels[i]->reset();
            }
        }
    }
    emitQueue();
    pump();
}

void DownloadEngine::pauseAll()
{
    {
        QMutexLocker lock(&m_mutex);
        for (int i = 0; i < m_tasks.size(); ++i) {
            if (m_tasks[i].status == TaskStatus::Downloading
                || m_tasks[i].status == TaskStatus::Pending) {
                m_tasks[i].status = TaskStatus::Paused;
                m_tasks[i].speedBps = 0;
                if (m_cancels[i]) m_cancels[i]->cancel();   // worker 在回调里轮询到就该退出
            }
        }
    }
    emitQueue();
}

void DownloadEngine::cancelTask(const QString& id)
{
    {
        QMutexLocker lock(&m_mutex);
        for (int i = 0; i < m_tasks.size(); ++i) {
            if (m_tasks[i].id != id) continue;
            m_tasks[i].status = TaskStatus::Canceled;
            if (m_cancels[i]) m_cancels[i]->cancel();
            break;
        }
    }
    emitQueue();
}

void DownloadEngine::removeTask(const QString& id)
{
    QMutexLocker lock(&m_mutex);
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].id != id) continue;
        if (m_running[i]) { if (m_cancels[i]) m_cancels[i]->cancel(); }
        m_tasks.remove(i);
        m_cancels.remove(i);
        m_running.remove(i);
        break;
    }
    lock.unlock();
    emitQueue();
}

void DownloadEngine::clearDone()
{
    QMutexLocker lock(&m_mutex);
    for (int i = m_tasks.size() - 1; i >= 0; --i) {
        if (m_tasks[i].status == TaskStatus::Done) {
            m_tasks.remove(i);
            m_cancels.remove(i);
            m_running.remove(i);
        }
    }
    lock.unlock();
    emitQueue();
}

void DownloadEngine::clearAll()
{
    QMutexLocker lock(&m_mutex);
    for (int i = 0; i < m_tasks.size(); ++i)
        if (m_running[i] && m_cancels[i]) m_cancels[i]->cancel();
    m_tasks.clear();
    m_cancels.clear();
    m_running.clear();
    lock.unlock();
    emitQueue();
}

void DownloadEngine::retryFailed()
{
    startAll();
}

void DownloadEngine::emitQueue()
{
    emit queueUpdated(queueJson());
}

void DownloadEngine::emitLibrary()
{
    const QVector<LibraryItem> lib = Database::instance().library(m_userId);
    QJsonArray arr;
    for (const LibraryItem& it : lib) arr.append(it.toJson());
    emit libraryUpdated(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

// ---------------------------------------------------------------------------
// 调度：填满空闲槽位
// ---------------------------------------------------------------------------
void DownloadEngine::pump()
{
    QVector<int> toStart;
    {
        QMutexLocker lock(&m_mutex);
        if (m_pumping) return;               // 防重入
        m_pumping = true;
        for (int i = 0; i < m_tasks.size() && m_active < m_concurrency; ++i) {
            if (m_running[i]) continue;
            if (m_tasks[i].status != TaskStatus::Pending) continue;
            m_running[i] = true;
            m_tasks[i].status = TaskStatus::Downloading;
            ++m_active;
            toStart.append(i);
        }
        m_pumping = false;
    }

    if (toStart.isEmpty()) return;
    emitQueue();

    for (int idx : toStart) {
        runAsync([this, idx]() { runTask(idx); });
    }
}

// ---------------------------------------------------------------------------
// ★ 单个任务的完整生命周期
// ---------------------------------------------------------------------------
DownloadEngine::Outcome DownloadEngine::downloadOne(
    const DownloadTask& task,
    const std::function<void(int, qint64)>& onProgress,
    const CancelTokenPtr& cancel)
{
    Outcome out;
    Settings& cfg = Settings::instance();

    // ---- 0) 下载前重新解析平台直链 ----
    // 为什么必须重来一次：平台给的直链是带时效的（网易云 expi=1200 秒，QQ 的
    // vkey 同理）。搜索结果里那条 URL 传到队列、排队、等前面几首下完，
    // 很可能已经过期 —— 表现就是"点了下载，过一会儿失败 403"。
    // 所以队列里只存"平台曲目 id"，地址在真正要下的这一刻现取。
    QString     directUrl = task.url;
    QStringList dlHeaders;
    Quality     effQuality = task.quality;
    QString     forcedExt;
    qint64      declaredSize = 0;
    // 平台拒绝直链后的换源兜底开关：
    //   false = 正常流程；true = 直链已被平台拒绝（要会员/无版权），
    //           落到 yt-dlp 搜索路径时必须用"关键词搜索"，绝不能再碰旧直链。
    bool        forceSearchFallback = false;
    QString     fallbackReason;

    if (PlatformResolve::supports(task.sourceId) && !task.platformId.isEmpty()) {
        const QString ck = task.sourceId == QLatin1String("netease")
                               ? Settings::instance().neteaseCookie()
                               : Settings::instance().qqCookie();
        const DirectAudio a = task.sourceId == QLatin1String("netease")
                                  ? PlatformResolve::netease(task.platformId, ck)
                                  : PlatformResolve::qq(task.platformId, ck);
        if (!a.ok) {
            // 平台拒绝了（要会员/无版权/未返回地址）。以前这里直接判死，
            // 用户点一首 QQ 会员歌就是"下载失败"，没有任何转圜。
            // 现在改成换源兜底：清空直链，落到下面的 yt-dlp 搜索路径，
            // 用"歌手 + 歌名"在哔哩哔哩上找同名曲目下载。找不到才真失败。
            qInfo() << "[Download]" << task.title
                    << "平台直链失败:" << (a.error.isEmpty() ? QStringLiteral("平台未提供可下载地址") : a.error)
                    << "→ 尝试换源下载";
            directUrl.clear();
            dlHeaders.clear();
            forcedExt.clear();
            declaredSize   = 0;
            effQuality     = task.quality;
            forceSearchFallback = true;
            fallbackReason = a.error.isEmpty() ? QStringLiteral("平台未提供可下载地址") : a.error;
        } else {
            directUrl    = a.url;
            dlHeaders    = PlatformResolve::headersFor(task.sourceId);
            forcedExt    = a.extension();
            declaredSize = a.sizeBytes;
            if (a.quality() != Quality::Unknown) effQuality = a.quality();
            qInfo() << "[Download]" << task.title << "已换到平台直链（"
                    << a.type << "/" << (a.br / 1000) << "kbps）";
        }
    }

    // ---- 1) 决定目标扩展名 ----
    // 优先无损：平台上给的就是 FLAC 直链时直接落盘，绝不二次编码
    QString ext = forcedExt;
    if (ext.isEmpty()) {
        ext = qualityKey(effQuality);
        if (ext == QLatin1String("unknown")) ext = cfg.preferQuality();
        if (ext == QLatin1String("320") || ext == QLatin1String("192")
            || ext == QLatin1String("128"))
            ext = QStringLiteral("mp3");
        if (ext == QLatin1String("hires")) ext = QStringLiteral("flac");
    }

    const QString savePath = buildSavePath(task, ext);
    if (QFileInfo::exists(savePath) && QFileInfo(savePath).size() > 64 * 1024) {
        // 已存在且体积正常：视为已完成（断点重跑场景）
        //
        // 这条早退**必须留日志**。在换源链路里它发生在"平台直链失败"之后、
        // 候选打分之前，不打日志的话日志上就是「尝试换源下载」下一行直接
        // 「下载成功」，中间没有任何检索/打分痕迹 —— 看着像换源秒成，
        // 其实只是旧文件被认领了（探针实测：间隔 2 毫秒）。
        // 换过评分或检索逻辑后想验证效果，一定先清掉目标文件再测，
        // 否则测到的是上一次留下的旧产物。
        qInfo() << "[Download] 目标文件已存在，跳过下载:" << savePath
                << "|" << QFileInfo(savePath).size() << "字节";
        out.ok = true;
        out.filePath = savePath;
        return out;
    }

    // ---- 2) 两条下载路径 ----
    Throttle throttle(100);

    // 判断一个 URL 是不是"平台网页"而不是音频直链：yt-dlp 的源给的是页面地址。
    auto isPageUrl = [](const QString& u) {
        if (!u.startsWith(QLatin1String("http://")) && !u.startsWith(QLatin1String("https://")))
            return false;
        static const char* kAudioExt[] = { ".mp3", ".flac", ".m4a", ".aac", ".wav", ".ape" };
        for (const char* e : kAudioExt)
            if (u.contains(QLatin1String(e), Qt::CaseInsensitive)) return false;
        return true;
    };

    if (!directUrl.isEmpty() && !isPageUrl(directUrl)) {
        // 2a) 音频直链：HTTP 直下，零转码
        const HttpClient::DownloadResult r = HttpClient::download(
            directUrl, savePath,
            [&](qint64 done, qint64 total) -> bool {
                if (cancel && cancel->isCanceled()) return false;
                if (onProgress && throttle.ready())
                    onProgress(total > 0 ? static_cast<int>(done * 100 / total) : 0, 0);
                return true;
            },
            cancel, dlHeaders, 0, /*allowResume=*/true);

        if (r.canceled) { out.error = QStringLiteral("已取消"); return out; }
        if (!r.ok)      { out.error = r.error; return out; }
        out.filePath = savePath;

        // 实得字节数比平台声明少一大截 → 拿到的是残包/试听片段，不能算成功。
        // 这一步很关键：宁可报"文件不完整"，也不要让用户以为下到了全曲。
        if (declaredSize > 0) {
            const qint64 got = QFileInfo(savePath).size();
            if (got < declaredSize * 0.97) {
                QFile::remove(savePath);
                out.error = QStringLiteral("下到的文件不完整（%1 / %2 字节），可能被 CDN 截断")
                                .arg(got).arg(declaredSize);
                return out;
            }
        }
    } else {
        // 2b) yt-dlp：平台页面 URL（首选）或搜索关键词（兜底）
        YtDlpWrapper yt;
        if (!yt.available() || !yt.healthy()) {
            out.error = yt.unavailableReason();
            out.fatal = true;
            return out;
        }
        YtDlpWrapper::DownloadOptions opt;
        opt.outputDir  = QFileInfo(savePath).absolutePath();
        opt.outputTemplate = sanitizeFileName(
            task.artist.isEmpty() ? task.title : task.artist + QStringLiteral(" - ") + task.title)
            + QStringLiteral(".%(ext)s");

        if (effQuality == Quality::Lossless || effQuality == Quality::HiRes) {
            opt.audioFormat  = QStringLiteral("flac");
            opt.audioQuality = QStringLiteral("0");       // 0 = 最高质量
        } else {
            opt.audioFormat  = QStringLiteral("mp3");
            opt.audioQuality = QStringLiteral("320K");
        }

        // 下载目标：
        //   有页面 URL（搜索结果里那条）→ 直接用它。搜索阶段已经精确到了那一首，
        //   再拿"歌手 + 歌名"去搜一次反而容易撞到翻唱/现场版。
        //   否则（换源兜底 / 任务只带关键词）→ 走候选打分选优，见 rankWebCandidates。
        auto tryDownload = [&](const QString& target) {
            return yt.download(target, opt,
                               [&](const YtDlpWrapper::ProgressUpdate& u) {
                                   if (onProgress) onProgress(static_cast<int>(u.percent), u.speedBps);
                               },
                               cancel);
        };

        YtDlpWrapper::DownloadResult r;
        QString pickedTitle, pickedNote;

        if (!forceSearchFallback && isPageUrl(task.url)) {
            r = tryDownload(task.url);

            // 页面地址失效了（视频被删/地区限制/需要登录）：退一步用候选搜索。
            // 以前这一步直接判失败，用户只能自己重搜一遍。
            if (!r.ok && !r.canceled) {
                qInfo() << "[Download]" << task.title
                        << "页面地址下载失败:" << r.error << "→ 改用候选搜索";
                forceSearchFallback = true;
                fallbackReason = QStringLiteral("页面地址不可用");
            }
        }

        if (!r.ok && !r.canceled) {
            const QVector<WebPick> cands = rankWebCandidates(task, cancel);
            if (cands.isEmpty()) {
                out.error = forceSearchFallback
                                ? QStringLiteral("%1；换源也没搜到任何候选").arg(fallbackReason)
                                : QStringLiteral("找不到可用的下载来源（候选搜索无结果）");
                return out;
            }
            if (cands.first().rel < kMinFallbackRel) {
                // 宁可不下载，也不给用户一个错的版本（以前抓第一条就会这样）
                out.error = QStringLiteral("%1；换源只找到明显不匹配的候选"
                                           "（最接近的是《%2》，相关度 %3/100，低于 %4），"
                                           "已放弃以免下错版本")
                                .arg(fallbackReason.isEmpty() ? QStringLiteral("无可用直链")
                                                              : fallbackReason,
                                     cands.first().title)
                                .arg(static_cast<int>(cands.first().rel))
                                .arg(static_cast<int>(kMinFallbackRel));
                return out;
            }

            // 从最相关的一条开始试，最多试 2 条（第一条多能成功；
            // 失败通常是那条视频的音频流本身有问题，换一条往往就好了）
            const int tries = qMin(2, cands.size());
            for (int i = 0; i < tries; ++i) {
                qInfo() << "[Download]" << task.title << "换源下载第" << (i + 1)
                        << "候选:" << cands[i].title << "| 相关度"
                        << static_cast<int>(cands[i].rel);
                r = tryDownload(cands[i].pageUrl);
                if (r.ok || r.canceled) {
                    pickedTitle = cands[i].title;
                    break;
                }
                qInfo() << "[Download] 该候选下载失败:" << r.error;
            }
        }

        if (r.canceled) { out.error = QStringLiteral("已取消"); return out; }
        if (!r.ok) {
            out.error = forceSearchFallback
                            ? QStringLiteral("%1；换源下载也失败（%2）").arg(fallbackReason, r.error)
                            : r.error;
            return out;
        }
        out.filePath = r.outputFile;
        // 换源成功的，把话说清楚：下到的是换源版本、是哪一条、相关度多少。
        // 不写清楚的话，用户下次打开文件夹会疑惑"这不是我要的那版"。
        if (forceSearchFallback) {
            out.note = pickedTitle.isEmpty()
                           ? QStringLiteral("平台拒绝直链（%1），已自动换源下载").arg(fallbackReason)
                           : QStringLiteral("平台拒绝直链（%1），已换源下载《%2》")
                                 .arg(fallbackReason, pickedTitle);
            pickedNote = out.note;
        }
        // 保留给日志：这次换源到底选了哪一条
        if (!pickedTitle.isEmpty())
            qInfo() << "[Download] 换源完成:" << pickedTitle << "|" << pickedNote;
    }

    if (out.filePath.isEmpty() || !QFileInfo::exists(out.filePath)) {
        out.error = QStringLiteral("下载完成但文件不存在");
        return out;
    }

    // ---- 3) 元数据写入 ----
    if (cfg.writeMetadata()) {
        TagWriter::Meta meta;
        meta.title  = task.title;
        meta.artist = task.artist;
        meta.album  = task.album;
        meta.comment = QStringLiteral("Downloaded by MelodySync (仅供个人学习研究)");

        QByteArray cover;
        if (cfg.embedCover() && !task.coverUrl.isEmpty())
            cover = TagWriter::downloadCover(task.coverUrl);

        QString err;
        if (!TagWriter::write(out.filePath, meta, cover, &err))
            qInfo() << "[Download] 元数据写入跳过:" << err;
    }

    // ---- 4) 歌词（可选） ----
    if (cfg.fetchLyrics()) {
        // 说明：歌词抓取依赖具体平台接口，这里留出扩展点；
        //       未配置时直接跳过，不影响下载成功与否。
        qInfo() << "[Download] 歌词抓取未启用";
    }

    out.ok = true;
    return out;
}

void DownloadEngine::runTask(int index)
{
    DownloadTask task;
    CancelTokenPtr cancel;
    {
        QMutexLocker lock(&m_mutex);
        if (index < 0 || index >= m_tasks.size()) return;
        task = m_tasks[index];
        cancel = m_cancels[index];
    }

    const QString id = task.id;
    Throttle throttle(120);

    auto onProgress = [this, id, &throttle](int percent, qint64 speed) {
        runOnMain([this, id, percent, speed]() {
            emit progress(id, percent, speed);
        });
        Q_UNUSED(throttle)
    };

    const Settings& cfg = Settings::instance();
    const int maxRetry = cfg.retries();

    Outcome outcome;
    for (int attempt = 0; attempt <= maxRetry; ++attempt) {
        if (cancel && cancel->isCanceled()) break;
        outcome = downloadOne(task, onProgress, cancel);
        if (outcome.ok) break;
        if (cancel && cancel->isCanceled()) break;
        // 平台已明确拒绝（要会员/无版权）：重试毫无意义，直接收工。
        // 以前这里不看原因，一律重试 —— 用户日志里于是出现大量
        // "第 1 次重试 / 第 2 次重试"，把真正的原因淹掉了。
        if (outcome.fatal) {
            qInfo() << "[Download]" << task.title << "不可重试:" << outcome.error;
            break;
        }
        if (attempt < maxRetry) {
            const int delay = 800 * (attempt + 1);      // 线性退避：0.8s / 1.6s
            qInfo() << "[Download]" << task.title << "第" << (attempt + 1) << "次重试";
            QThread::msleep(static_cast<unsigned long>(delay));
        }
    }

    // ---- 落库 + 状态回写（全部在主线程里做，避免和 UI 读状态打架）----
    runOnMain([this, index, id, task, outcome, cancel]() {
        bool stillPausedOrCanceled = false;
        {
            QMutexLocker lock(&m_mutex);
            if (index >= m_tasks.size() || m_tasks[index].id != id) return;
            DownloadTask& t = m_tasks[index];
            m_running[index] = false;
            if (m_active > 0) --m_active;

            if (cancel && cancel->isCanceled()) {
                // 用户主动暂停/取消：保留用户选择的状态，不覆盖成 Failed
                stillPausedOrCanceled = true;
                t.speedBps = 0;
            } else if (outcome.ok) {
                t.status = TaskStatus::Done;
                t.percent = 100;
                t.speedBps = 0;
                t.savePath = outcome.filePath;
                t.totalBytes = QFileInfo(outcome.filePath).size();
            } else {
                t.status = TaskStatus::Failed;
                t.error = outcome.error;
                t.speedBps = 0;
            }
        }

        if (outcome.ok && !stillPausedOrCanceled) {
            // 写入音乐库
            LibraryItem item;
            item.title     = task.title;
            item.artist    = task.artist;
            item.album     = task.album;
            item.filePath  = outcome.filePath;
            item.sizeBytes = QFileInfo(outcome.filePath).size();
            item.quality   = task.quality;
            item.format    = QFileInfo(outcome.filePath).suffix().toLower();
            item.addedAt   = nowIso();
            TagWriter::Meta m;
            if (TagWriter::read(outcome.filePath, &m)) {
                if (!m.title.isEmpty())  item.title  = m.title;
                if (!m.artist.isEmpty()) item.artist = m.artist;
                if (!m.album.isEmpty())  item.album  = m.album;
                item.year    = m.year;
                item.trackNo = m.trackNo;
                item.genre   = m.genre;
            }
            if (!Database::instance().libraryHasFile(m_userId, item.filePath))
                Database::instance().addLibraryItem(m_userId, item);

            DownloadTask done = task;
            done.status = TaskStatus::Done;
            done.savePath = outcome.filePath;
            done.totalBytes = item.sizeBytes;
            Database::instance().addHistory(m_userId, done);

            emit toast(outcome.note.isEmpty()
                           ? QStringLiteral("《%1》下载完成").arg(task.title)
                           : QStringLiteral("《%1》下载完成：%2").arg(task.title, outcome.note),
                       QString());
            emit taskFinished(id, outcome.filePath);
            emitLibrary();
        } else if (!stillPausedOrCanceled) {
            emit toast(QStringLiteral("《%1》下载失败：%2").arg(task.title, outcome.error),
                       QStringLiteral("e"));
            DownloadTask failed = task;
            failed.status = TaskStatus::Failed;
            failed.error = outcome.error;
            Database::instance().addHistory(m_userId, failed);
        }

        emitQueue();
        pump();     // 腾出槽位，继续下一个
    });
}

} // namespace ms
