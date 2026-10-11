// ============================================================================
//  src/download/YtDlpWrapper.cpp
// ============================================================================
#include "download/YtDlpWrapper.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>

namespace ms {

namespace {
QString g_customPath;

// yt-dlp 的进度行样例：
//   [download]  45.3% of   10.20MiB at  1.20MiB/s ETA 00:05
//   [download] 100% of    3.45MiB in 00:03
// 注意：百分比与 "at" 之间可能是多个空格，"~" 表示文件总大小是估算值。
const QRegularExpression& progressRe()
{
    static const QRegularExpression re(QStringLiteral(
        "\\[download\\]\\s+([0-9.]+)%\\s+of\\s+~?\\s*([0-9.]+)(KiB|MiB|GiB|B)"
        "(?:\\s+at\\s+~?\\s*([0-9.]+)(KiB|MiB|GiB|B)/s)?"));
    return re;
}

const QRegularExpression& destRe()
{
    // [download] Destination: C:\Music\xxx.webm
    // [ExtractAudio] Destination: C:\Music\xxx.flac
    static const QRegularExpression re(QStringLiteral(
        "\\[(?:download|ExtractAudio|Merger|ffmpeg)\\]\\s+Destination:\\s*(.+)$"));
    return re;
}

double toBytes(double v, const QString& unit)
{
    if (unit == QLatin1String("KiB")) return v * 1024.0;
    if (unit == QLatin1String("MiB")) return v * 1024.0 * 1024.0;
    if (unit == QLatin1String("GiB")) return v * 1024.0 * 1024.0 * 1024.0;
    return v;
}

// ---------- 健康检查缓存 ----------
// 探测要起一个真实进程（几百毫秒），搜索结果页会连着调好几次，所以缓存起来。
// 只缓存"成功"：失败可能是暂时的（杀软正在隔离文件），下次还应该重试。
//   -1 未探测 / 0 不健康 / 1 健康
int g_health = -1;
QString g_healthReason;
QString g_healthExe;

// 一次只跑一个 yt-dlp。
// 搜索时最多 8 个源并行，其中 3 个（哔哩哔哩 / YouTube / SoundCloud）会各自
// 起一个 yt-dlp；下载侧并发也是 3。而这些 exe 是 PyInstaller 单文件包，
// **每个进程启动时都要把自带的上百个模块解压到 %TEMP%**，
// 几个进程同时解压会互相抢磁盘，实测出现过其中一路拿到空输出、
// 报出一句没有任何信息的"搜索失败"（错误串是空的，日志里完全看不出原因）。
// 所以串行化保留。但朴素的 QMutex 有两个致命问题（2026-09-19"搜索卡死"根因）：
//   1) **等待不可取消**——总超时触发 cancel 后，还排在队列里的源要傻等轮到
//      自己才第一次有机会检查 cancel，聚合器的 join 被拖满整个队列时长；
//   2) **无优先级**——YouTube 在无代理环境一次搜索要挂 30~44 秒，把排在
//      后面的哔哩哔哩（国内唯一稳定可出结果的 yt-dlp 源）活活饿死到总超时。
// 因此改成"带优先级 + 可取消等待"的闸门：
//   哔哩哔哩搜索(0) > 其他 yt-dlp 搜索(1) > 下载(2)。
// 等待期间每 60ms 轮询一次 cancel，取消即刻退出队列、绝不再启动进程。
class ProcGate
{
public:
    // 拿到执行权返回 true；排队等待期间被取消返回 false（此时绝不持有闸门）。
    bool acquire(int priority, const CancelTokenPtr& cancel)
    {
        QMutexLocker l(&m_m);
        const quint64 seq = m_nextSeq++;
        m_waiters.append({ priority, seq });
        l.unlock();

        while (true) {
            if (cancel && cancel->isCanceled()) {
                l.relock();
                removeWaiterLocked(seq);
                return false;
            }
            l.relock();
            // (priority, seq) 最小者获准执行：同优先级按先来后到
            if (!m_busy && isChosenLocked(seq)) {
                m_busy = true;
                removeWaiterLocked(seq);
                return true;
            }
            l.unlock();
            QThread::msleep(60);
        }
    }

    void release()
    {
        QMutexLocker l(&m_m);
        m_busy = false;
    }

private:
    struct Waiter { int priority; quint64 seq; };

    void removeWaiterLocked(quint64 seq)
    {
        for (int i = 0; i < m_waiters.size(); ++i)
            if (m_waiters.at(i).seq == seq) { m_waiters.removeAt(i); return; }
    }

    bool isChosenLocked(quint64 seq) const
    {
        if (m_waiters.isEmpty()) return false;
        int best = 0;
        for (int i = 1; i < m_waiters.size(); ++i) {
            const Waiter& a = m_waiters.at(i);
            const Waiter& b = m_waiters.at(best);
            if (a.priority < b.priority || (a.priority == b.priority && a.seq < b.seq))
                best = i;
        }
        return m_waiters.at(best).seq == seq;
    }

    QMutex m_m;
    bool m_busy = false;
    quint64 m_nextSeq = 0;
    QList<Waiter> m_waiters;
};

ProcGate g_procGate;

enum ProcPrio { kPrioBiliSearch = 0, kPrioSearch = 1, kPrioDownload = 2 };

// 闸门的 RAII 归还：acquire 成功后必须构造一个，保证所有 return 路径都放行。
struct ProcGateGuard {
    ProcGate* gate;
    ~ProcGateGuard() { gate->release(); }
};

// 搜索优先级：哔哩哔哩是国内唯一稳定可用的 yt-dlp 源，必须最先跑。
// （YouTube 不可达时一次搜索要挂 30~44 秒，绝不能让它排在哔哩哔哩前面。）
int searchPriorityFor(const QString& prefix)
{
    return prefix.startsWith(QLatin1String("bilisearch")) ? kPrioBiliSearch : kPrioSearch;
}

// 站点需要"看起来像浏览器"时用的请求头。
// 哔哩哔哩对没有 UA / Referer 的请求会回 HTTP 412（Precondition Failed），
// 而 yt-dlp 的原话是 "Unable to download JSON metadata: HTTP Error 412"，
// 完全看不出是请求头的问题。实测同一个中文查询：
//   裸跑                    → 412，0 条结果
//   加 --referer            → 正常返回
//   加 --user-agent         → 正常返回
// 两个都加上，成功率最高（412 本身是间歇性的风控，加了头之后基本不再出现）。
const char* kBrowserUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

QString refererFor(const QString& target)
{
    if (target.startsWith(QLatin1String("bilisearch")) || target.contains(QLatin1String("bilibili.com")))
        return QStringLiteral("https://www.bilibili.com/");
    if (target.startsWith(QLatin1String("scsearch")) || target.contains(QLatin1String("soundcloud.com")))
        return QStringLiteral("https://soundcloud.com/");
    if (target.contains(QLatin1String("youtube.com")) || target.startsWith(QLatin1String("ytsearch")))
        return QStringLiteral("https://www.youtube.com/");
    return QString();
}

// 哔哩哔哩除了 UA/Referer 之外还认 buvid3 cookie：实测同一个 bilisearch 下载，
// 只有 UA+Referer 时仍可能 412（Precondition Failed），补上一个任意值的
// buvid3 就稳定通过（这就是 B 站风控对"新设备"的最低要求）。
// yt-dlp 会对 header 里带 cookie 打一条 deprecation 警告（会自动限定到
// bilibili 域，无实际风险），--no-warnings 下不会刷屏。
bool needsBiliCookie(const QString& target)
{
    return target.startsWith(QLatin1String("bilisearch"))
           || target.contains(QLatin1String("bilibili.com"));
}
const char* kBiliCookie = "Cookie: buvid3=BD2F05B5-4E8A-4F2B-9E3D-0A1B2C3D4E5Finfoc;";

// 哪些**原始 stderr** 值得整轮重试一次。
//
// 一开始写的是"任何失败都重试"，结果把不可达的源的时间成本翻了一倍：
// YouTube 经代理连不上时单次要 80 秒（yt-dlp 内部默认重试 10 次），
// 外面再重试一次就是 160 秒 —— 用户会以为程序死了。
// 而"连不上"重试一次的结果和第一次完全一样，纯属白等。
//
// 注意：这里判的是 yt-dlp 的**原始输出**，不是我们翻译过的中文文案 ——
// 拿自己写的提示语做匹配，哪天改个措辞这条逻辑就静默失效了。
bool retryableRawError(const QString& raw)
{
    if (raw.contains(QLatin1String("HTTP Error 412"))) return true;   // 风控限流，间歇性
    if (raw.contains(QLatin1String("HTTP Error 403"))) return true;
    // 网络不通 / 代理失败 / DNS / 超时：重试没有意义
    if (raw.contains(QLatin1String("Unable to connect to proxy"))) return false;
    if (raw.contains(QLatin1String("Tunnel connection failed"))) return false;
    if (raw.contains(QLatin1String("Failed to resolve"))) return false;
    if (raw.contains(QLatin1String("getaddrinfo"))) return false;
    if (raw.contains(QLatin1String("Connection refused"))) return false;
    if (raw.contains(QLatin1String("timed out"))) return false;
    if (raw.contains(QLatin1String("Unable to download webpage"))
        || raw.contains(QLatin1String("Unable to download API page"))) return false;
    // 其余（解析类、格式类）重试一次通常也没用，但代价低，就让它试
    return true;
}

// flat-playlist 模式下 yt-dlp 的 url 字段常常只是站点内 id（YouTube 是 videoId，
// SoundCloud 是页面路径）。直接把它塞给下载器会得到一个无效目标，
// 所以这里按搜索前缀补全成可访问的页面地址。
QString canonicalPageUrl(const QString& prefix, const QString& id, const QString& rawUrl)
{
    const bool looksLikeUrl = rawUrl.startsWith(QLatin1String("http://"))
                              || rawUrl.startsWith(QLatin1String("https://"));
    if (looksLikeUrl) return rawUrl;
    const QString key = id.isEmpty() ? rawUrl : id;
    if (key.isEmpty()) return QString();

    if (prefix.startsWith(QLatin1String("scsearch")))
        return key.startsWith(QLatin1String("http")) ? key
                                                     : QStringLiteral("https://soundcloud.com/") + key;
    // 默认按 YouTube 处理
    return QStringLiteral("https://www.youtube.com/watch?v=") + key;
}
} // namespace

// ---------------------------------------------------------------------------
YtDlpWrapper::YtDlpWrapper()
{
    m_exe = executablePath();
}

void YtDlpWrapper::setCustomPath(const QString& path) { g_customPath = path; }

QString YtDlpWrapper::executablePath()
{
    if (!g_customPath.isEmpty() && QFileInfo::exists(g_customPath))
        return g_customPath;

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/bin/yt-dlp.exe"),
        appDir + QStringLiteral("/yt-dlp.exe"),
        appDir + QStringLiteral("/../bin/yt-dlp.exe"),           // 构建目录布局
        QDir::homePath() + QStringLiteral("/.melodysync/bin/yt-dlp.exe")
    };
    for (const QString& c : candidates)
        if (QFileInfo::exists(c)) return QFileInfo(c).absoluteFilePath();

    // 最后回落到 PATH（用户自己装过 yt-dlp 的情况）
    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
    return fromPath;
}

QString YtDlpWrapper::version() const
{
    if (!available()) return QString();
    QProcess p;
    p.start(m_exe, { QStringLiteral("--version") });
    if (!p.waitForFinished(8000)) return QString();
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// ---------------------------------------------------------------------------
bool YtDlpWrapper::healthy()
{
    const QString exe = executablePath();
    if (exe.isEmpty()) return false;
    if (g_health == 1 && g_healthExe == exe) return true;
    if (g_health == 0 && g_healthExe == exe) return false;

    g_healthExe = exe;
    QProcess p;
    p.start(exe, { QStringLiteral("--version") });
    if (!p.waitForStarted(8000)) {
        g_health = 0;
        g_healthReason = QStringLiteral("yt-dlp.exe 无法启动（可能被杀毒软件拦截）");
        return false;
    }
    // 超时给到 45 秒，并且要区分"慢"和"坏"：
    //   - 文件损坏时 yt-dlp 是**立刻**失败并打出 PYI- 报错，不会慢；
    //   - 真正会慢的是**首次**启动 —— PyInstaller 单文件包要把自带的上百个模块
    //     解压到 %TEMP%，硬盘慢或杀软实时扫描时能跑好几分钟。
    // 一开始写的是 12 秒：那会把"第一次有点慢"直接判成"已损坏"，
    // 用户在界面上看到"yt-dlp.exe 已损坏，请重新下载"，而文件其实是好的。
    // 结果有缓存（g_health），所以这个等待整个过程只付一次。
    p.waitForFinished(45000);
    const QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();

    if (QRegularExpression(QStringLiteral("^\\d{4}\\.\\d{2}\\.\\d{2}")).match(out).hasMatch()) {
        g_health = 1;
        g_healthReason.clear();
        qInfo() << "[yt-dlp] 健康检查通过:" << out.split(QLatin1Char('\n')).value(0);
        return true;
    }

    g_health = 0;
    // PyInstaller 单文件包自解压失败是最常见的一种：文件被下载截断 / 杀软隔离。
    // 这个报错本身完全看不懂，必须换成可操作的话。
    if (err.contains(QLatin1String("Failed to extract"))
        || err.contains(QLatin1String("PYI-"))) {
        g_healthReason =
            QStringLiteral("yt-dlp.exe 已损坏（自解压失败）。请重新运行 tools/fetch_ytdlp.py 覆盖下载。");
    } else {
        g_healthReason = QStringLiteral("yt-dlp.exe 无法运行：%1")
                             .arg((err.isEmpty() ? out : err).left(200));
    }
    qWarning() << "[yt-dlp] 健康检查失败:" << g_healthReason;
    return false;
}

QString YtDlpWrapper::unavailableReason()
{
    if (executablePath().isEmpty())
        return QStringLiteral("未找到 yt-dlp.exe");
    if (!healthy()) return g_healthReason;
    return QString();
}

void YtDlpWrapper::prewarm()
{
    // 在后台线程先把健康检查做掉。
    // 为什么必须做：首次探测可能要几十秒（见 healthy() 里的说明），
    // 而 getSources() 是**在 UI 线程**被前端调用的 —— 不预热的话，
    // 首屏打开「设置 → 搜索源」会整段卡死。
    runAsync([]() { YtDlpWrapper::healthy(); });
}

// ---------------------------------------------------------------------------
QString YtDlpWrapper::humanizeError(const QString& rawStdErr, int exitCode)
{
    const QString e = rawStdErr;
    if (e.contains(QLatin1String("Failed to extract")) || e.contains(QLatin1String("PYI-")))
        return QStringLiteral("yt-dlp.exe 已损坏（自解压失败），请重新运行 tools/fetch_ytdlp.py");
    if (e.contains(QLatin1String("Video unavailable")) || e.contains(QLatin1String("This video is not available")))
        return QStringLiteral("该视频在当前地区不可用（YouTube 通常需要能直连的网络）");
    if (e.contains(QLatin1String("Sign in to confirm")) || e.contains(QLatin1String("bot")))
        return QStringLiteral("平台要求验证登录态，yt-dlp 无法直接抓取");
    if (e.contains(QLatin1String("HTTP Error 412")))
        return QStringLiteral("平台风控拦截（HTTP 412），这是哔哩哔哩的限流，稍后重试即可");
    if (e.contains(QLatin1String("HTTP Error 403")))
        return QStringLiteral("平台拒绝了请求（403），链接可能已过期，请重试");
    if (e.contains(QLatin1String("Unable to connect to proxy"))
        || e.contains(QLatin1String("Tunnel connection failed")))
        return QStringLiteral("通过代理连接失败（Tunnel 报错）：请检查系统代理设置或稍后重试");
    if (e.contains(QLatin1String("Unable to download webpage"))
        || e.contains(QLatin1String("Unable to download API page"))
        || e.contains(QLatin1String("Failed to resolve"))
        || e.contains(QLatin1String("getaddrinfo"))
        || e.contains(QLatin1String("Connection refused"))
        || e.contains(QLatin1String("timed out")))
        return QStringLiteral("无法连接目标站点（网络不通或被墙）。YouTube 需要能直连的网络，"
                              "国内可用哔哩哔哩源代替");
    if (e.contains(QLatin1String("No such file or directory")) && e.contains(QLatin1String("ffmpeg")))
        return QStringLiteral("缺少 ffmpeg，无法转码；请确认 exe 同级有 ffmpeg.exe");

    // 实在不认识：把最后一行原文附上，总比只说一个退出码强
    const QStringList lines = e.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                      Qt::SkipEmptyParts);
    const QString tail = lines.isEmpty() ? QString() : lines.last().trimmed().left(200);
    return tail.isEmpty() ? QStringLiteral("yt-dlp 退出码 %1").arg(exitCode)
                          : QStringLiteral("yt-dlp 失败：%1").arg(tail);
}

// ---------------------------------------------------------------------------
bool YtDlpWrapper::search(const QString& query, int limit,
                          QVector<SearchItem>* out, const QString& prefix,
                          QString* errorOut, const CancelTokenPtr& cancel,
                          bool flat)
{
    if (!available()) {
        if (errorOut) *errorOut = QStringLiteral("未找到 yt-dlp.exe");
        return false;
    }
    // 文件在 ≠ 能跑。截断的 exe 会让这个源每次都"搜到 0 条"，
    // 日志里却只有一句含糊的失败，所以这里先做健康检查。
    if (!healthy()) {
        if (errorOut) *errorOut = unavailableReason();
        return false;
    }

    // ytsearchN: / scsearchN: / bilisearchN: 是 yt-dlp 的搜索伪协议：
    // 等价于在站点搜索框里输入关键词，只取搜索结果页的元数据。
    const QString target = QStringLiteral("%1%2:%3").arg(prefix).arg(limit).arg(query);

    QStringList args;
    args << target;
    if (flat) {
        // 不解析每个视频的详情，速度提升 10 倍以上。
        // 前提是站点在搜索结果里就带了标题 —— 哔哩哔哩不带，见头文件说明。
        args << QStringLiteral("--flat-playlist");
    } else {
        // 非 flat 时必须禁掉歌单展开。
        // 不然后果很夸张：搜索结果里只要有一条是"合集"，yt-dlp 就会把它整个
        // 展开成几十条（实测一个 20 首的合集直接灌进 30+ 条，标题全是
        // "… p17 17.xxx"），把真正相关的原唱结果全挤到后面去。
        // 加了 --no-playlist，每条搜索结果就只对应一条视频（实测稳定 12 条）。
        args << QStringLiteral("--no-playlist");
    }
    args << QStringLiteral("--dump-json")         // 每行一个 JSON
         << QStringLiteral("--no-warnings")
         << QStringLiteral("--ignore-config")
         << QStringLiteral("--no-check-certificates")
         << QStringLiteral("--socket-timeout") << QStringLiteral("10")
         // 收紧 yt-dlp **内部**的重试次数（默认 10 次）。
         // 实测：目标站点不可达时，默认重试要跑 84 秒才放弃；
         // 改成 2/1 之后是 44 秒。搜索是交互操作，不能让人等一分半。
         // 下载那条路径不动（大文件断续需要重试，可靠性优先）。
         << QStringLiteral("--retries") << QStringLiteral("2")
         << QStringLiteral("--extractor-retries") << QStringLiteral("1")
         // 浏览器化的请求头：哔哩哔哩的风控会对"裸"请求回 412（见 kBrowserUA 的说明）
         << QStringLiteral("--user-agent") << QString::fromLatin1(kBrowserUA);
    const QString ref = refererFor(prefix);
    if (!ref.isEmpty()) args << QStringLiteral("--referer") << ref;
    if (needsBiliCookie(target))
        args << QStringLiteral("--add-headers") << QString::fromLatin1(kBiliCookie);

    // 单次尝试。抽成 lambda 是因为 412 / 超时都是**间歇性**的，值得重试一次。
    auto runOnce = [&](QString* errOut, bool* retryableOut) -> int {
        // 带优先级 + 可取消的排队：等待期间 cancel 一旦置位立即退出，
        // 绝不启动新进程（否则总超时后 join 还要再等一整轮 yt-dlp）。
        if (!g_procGate.acquire(searchPriorityFor(prefix), cancel)) {
            qInfo() << "[yt-dlp] 排队等待中被取消，跳过本次搜索:" << target;
            return -1;
        }
        ProcGateGuard gateGuard{ &g_procGate };

        if (cancel && cancel->isCanceled()) return -1;   // 恰好轮到时已被取消

        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        // 不写死编码的话，Python 在 Windows 上会把中文按 ANSI 代码页输出，
        // 我们按 UTF-8 解出来就是乱码（日志里能看到成片的 ''）。
        env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
        p.setProcessEnvironment(env);
        p.start(m_exe, args);
        if (!p.waitForStarted(8000)) {
            if (errOut) *errOut = QStringLiteral("yt-dlp 启动失败");
            return 0;
        }

        // 边读边等：既避免大结果集把管道塞满导致死锁，
        // **也**顺手把 stderr 攒下来 —— 以前只在最后读一次，
        // 出现过"失败但错误串是空的"（日志里只有一句无信息量的「搜索失败: ""」）。
        QByteArray stdoutBuf, stderrBuf;
        bool killedByCancel = false;
        while (p.state() != QProcess::NotRunning) {
            if (cancel && cancel->isCanceled()) {
                // 总超时/用户取消：杀掉进程，但**继续往下解析已输出的行**。
                // yt-dlp 搜索是逐条把 JSON 打到 stdout 的，超时被杀时
                // 已经输出的条目仍然有效——表现为"部分结果"而不是整源作废
                // （哔哩哔哩全量解析较慢，18~20 秒预算下经常只来得及出一部分）。
                p.kill();
                p.waitForFinished(2000);
                killedByCancel = true;
                break;
            }
            p.waitForReadyRead(300);
            stdoutBuf.append(p.readAllStandardOutput());
            stderrBuf.append(p.readAllStandardError());
        }
        p.waitForFinished(5000);
        stdoutBuf.append(p.readAllStandardOutput());
        stderrBuf.append(p.readAllStandardError());

        int parsed = 0;
        for (const QByteArray& line : stdoutBuf.split('\n')) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.isEmpty() || !trimmed.startsWith('{')) continue;

            const QJsonObject o = QJsonDocument::fromJson(trimmed).object();
            SearchItem it;
            it.id          = o.value(QStringLiteral("id")).toString();
            it.title       = o.value(QStringLiteral("title")).toString();
            it.uploader    = o.value(QStringLiteral("uploader")).toString(
                                 o.value(QStringLiteral("channel")).toString());
            it.durationSec = o.value(QStringLiteral("duration")).toInt();
            // 热度：哔哩哔哩给 view_count/like_count；SoundCloud 用 play_count；
            // 有的站点两者都没有（flat 模式尤其常见）→ 保持 0，上层按"未知"处理。
            it.viewCount   = o.value(QStringLiteral("view_count")).toVariant().toLongLong();
            if (it.viewCount <= 0)
                it.viewCount = o.value(QStringLiteral("play_count")).toVariant().toLongLong();
            it.likeCount   = o.value(QStringLiteral("like_count")).toVariant().toLongLong();
            it.thumbnail   = o.value(QStringLiteral("thumbnail")).toString();
            it.url         = o.value(QStringLiteral("url")).toString(
                                 o.value(QStringLiteral("webpage_url")).toString());
            it.pageUrl     = canonicalPageUrl(prefix, it.id, it.url);
            if (it.title.isEmpty()) continue;
            if (it.pageUrl.isEmpty()) continue;
            out->append(it);
            ++parsed;
        }
        if (parsed > 0)
            qInfo() << "[yt-dlp] 解析出" << parsed << "条"
                    << (killedByCancel ? QStringLiteral("（超时中止，部分结果）")
                                       : QString());

        if (parsed == 0 && errOut) {
            if (killedByCancel) {
                // 被总超时杀掉且一条都没来得及输出：这不是站点的问题，
                // 不值得重试（重试也会同样被超时杀掉）。
                *errOut = QStringLiteral("搜索超时被中止，未得到结果");
                if (retryableOut) *retryableOut = false;
            } else {
            const QString raw = QString::fromUtf8(stderrBuf).trimmed();
            if (raw.isEmpty()) {
                // 有输出但一条都没解析出来：多半是站点在搜索结果里不带标题
                // （哔哩哔哩的 flat 模式就是这样）。这类不值得重试。
                *errOut = QStringLiteral("yt-dlp 返回了 %1 字节但解析不出任何曲目"
                                         "（该站点可能未在搜索结果里提供标题）")
                              .arg(stdoutBuf.size());
                if (retryableOut) *retryableOut = false;
            } else {
                *errOut = humanizeError(raw, p.exitCode());
                // 用**原始 stderr** 判定，不用翻译后的中文文案
                if (retryableOut) *retryableOut = retryableRawError(raw);
            }
            // 只在失败时打诊断：目标 + 退出码 + 字节数，够定位了
            qWarning() << "[yt-dlp] 搜索失败 目标=" << target
                       << " 退出码=" << p.exitCode()
                       << " stdout=" << stdoutBuf.size()
                       << " stderr=" << stderrBuf.size();
            }
        }
        return parsed;
    };

    // 先清空：重试时不能把上一次的残留结果和新结果混在一起
    const int before = out->size();
    QString err1;
    bool retryable = false;
    if (runOnce(&err1, &retryable) > 0) return true;

    if (cancel && cancel->isCanceled()) return false;
    out->resize(before);            // 不留半截结果

    // 只有在"值得重试"时才重试第二次。
    // 哔哩哔哩的 412 是间歇性的，多跑一次常常就好了；
    // 但"连不上 YouTube"重试多少次都一样，只会让用户白等一个周期。
    if (!retryable) {
        qInfo() << "[" << prefix << "] 失败（不可重试类，不再重试）：" << err1;
        if (errorOut) *errorOut = err1;
        return false;
    }

    qInfo() << "[" << prefix << "] 首次失败（可重试），重试一次：" << err1;
    QString err2;
    if (runOnce(&err2, nullptr) > 0) {
        qInfo() << "[" << prefix << "] 重试成功";
        return true;
    }
    out->resize(before);
    if (errorOut) *errorOut = err2.isEmpty() ? err1 : err2;
    return false;
}

// ---------------------------------------------------------------------------
bool YtDlpWrapper::parseProgressLine(const QString& line, ProgressUpdate* out)
{
    const QRegularExpressionMatch m = progressRe().match(line);
    if (!m.hasMatch()) return false;

    out->percent    = m.captured(1).toDouble();
    out->totalBytes = static_cast<qint64>(toBytes(m.captured(2).toDouble(), m.captured(3)));
    out->bytes      = static_cast<qint64>(out->totalBytes * out->percent / 100.0);
    if (!m.captured(4).isEmpty())
        out->speedBps = static_cast<qint64>(toBytes(m.captured(4).toDouble(), m.captured(5)));
    out->stage = QStringLiteral("下载中");
    return true;
}

YtDlpWrapper::DownloadResult YtDlpWrapper::download(
    const QString& target,
    const DownloadOptions& opt,
    const std::function<void(const ProgressUpdate&)>& onProgress,
    const CancelTokenPtr& cancel)
{
    DownloadResult res;
    if (!available()) { res.error = QStringLiteral("未找到 yt-dlp.exe"); return res; }
    if (!healthy())   { res.error = unavailableReason(); return res; }

    QDir().mkpath(opt.outputDir);

    QStringList args;
    args << target
         << QStringLiteral("-x")                                   // 只取音频
         << QStringLiteral("--ignore-config")
         << QStringLiteral("--no-playlist")
         << QStringLiteral("--newline")                            // 让进度按行输出，便于解析
         << QStringLiteral("--no-warnings")
         << QStringLiteral("--socket-timeout") << QStringLiteral("15")
         << QStringLiteral("-N") << QStringLiteral("4")            // 4 个分片并发拉取，明显提速
         << QStringLiteral("-P") << opt.outputDir
         << QStringLiteral("-o") << opt.outputTemplate;

    if (!opt.keepOriginal) {
        args << QStringLiteral("--audio-format") << opt.audioFormat
             << QStringLiteral("--audio-quality") << opt.audioQuality;
    }
    if (opt.embedThumbnail) args << QStringLiteral("--embed-thumbnail");
    if (opt.addMetadata)    args << QStringLiteral("--add-metadata");
    args << opt.extraArgs;

    // 和搜索一样要有浏览器化的头：哔哩哔哩的页面解析同样吃 412 风控
    args << QStringLiteral("--user-agent") << QString::fromLatin1(kBrowserUA);
    const QString ref = refererFor(target);
    if (!ref.isEmpty()) args << QStringLiteral("--referer") << ref;
    if (needsBiliCookie(target))
        args << QStringLiteral("--add-headers") << QString::fromLatin1(kBiliCookie);

    // ffmpeg 定位：-x 转码必须要 ffmpeg。我们把它和 yt-dlp.exe 一起部署在
    // 同级目录，但 yt-dlp 默认只查 PATH —— 用户 PATH 里没有时就会报
    // "缺少 ffmpeg，无法转码"。这里显式指过去，不赌环境。
    const QString exeDir = QFileInfo(m_exe).absolutePath();
    if (QFileInfo::exists(exeDir + QStringLiteral("/ffmpeg.exe")))
        args << QStringLiteral("--ffmpeg-location") << exeDir;

    // 关键：把最终文件路径打到 stdout，避免我们靠猜文件名
    args << QStringLiteral("--print") << QStringLiteral("after_move:filepath");

    // 与 search() 共用同一个闸门：一次只跑一个 yt-dlp 进程
    // （PyInstaller 包启动时要解压到 %TEMP%，多个进程同时解压会互抢磁盘）。
    // 下载优先级最低（kPrioDownload）：搜索是交互操作，不能被一个
    // 正在下载的大文件堵住几分钟；且排队等待同样可取消。
    if (!g_procGate.acquire(kPrioDownload, cancel)) {
        res.canceled = true;
        res.error = QStringLiteral("已取消");
        return res;
    }
    ProcGateGuard gateGuard{ &g_procGate };

    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    p.setProcessEnvironment(env);
    p.start(m_exe, args);
    if (!p.waitForStarted(10000)) {
        res.error = QStringLiteral("yt-dlp 启动失败");
        return res;
    }

    QString lastDest;
    QByteArray stdoutBuf;
    QByteArray stderrBuf;          // 保留：失败时这里是唯一能解释原因的线索
    ProgressUpdate upd;

    while (p.state() != QProcess::NotRunning) {
        if (cancel && cancel->isCanceled()) {
            p.kill();
            p.waitForFinished(3000);
            res.canceled = true;
            res.error = QStringLiteral("已取消");
            return res;
        }

        p.waitForReadyRead(200);

        // ---- stdout：主要解析 --print 输出的最终路径 ----
        const QString so = QString::fromUtf8(p.readAllStandardOutput());
        if (!so.isEmpty()) stdoutBuf.append(so.toUtf8());

        // ---- stderr/stdout 混合：yt-dlp 的进度信息走 stdout，这里统一扫 ----
        const QString combined = so;
        if (!combined.isEmpty()) {
            const QStringList lines = combined.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                                     Qt::SkipEmptyParts);
            for (const QString& line : lines) {
                const QRegularExpressionMatch dm = destRe().match(line);
                if (dm.hasMatch()) lastDest = dm.captured(1).trimmed();

                ProgressUpdate u;
                if (parseProgressLine(line, &u)) {
                    upd = u;
                    if (onProgress) onProgress(upd);
                } else if (line.contains(QLatin1String("[ExtractAudio]"))) {
                    upd.stage = QStringLiteral("转码中");
                    upd.percent = 100.0;
                    if (onProgress) onProgress(upd);
                } else if (line.contains(QLatin1String("[Metadata]"))
                           || line.contains(QLatin1String("[EmbedThumbnail]"))) {
                    upd.stage = QStringLiteral("处理元数据");
                    if (onProgress) onProgress(upd);
                }
            }
        }

        // stderr 攒起来：失败时这里是唯一能解释原因的线索
        // （原本这一行是直接丢弃的，于是用户只看到"yt-dlp 退出码 1"）
        stderrBuf.append(p.readAllStandardError());
    }

    p.waitForFinished(5000);
    // ⚠ 必须**追加到末尾**，不能写成 (tail + stdoutBuf) 把两段拼反。
    // 循环里是"边读边攒"，一段路径如果正好落在两次读取的边界上，
    // 只按正确顺序拼接才能还原成完整一行；拼反了那一行就断了，
    // 于是明明文件已经落盘，却"定位不到输出文件"。
    // （真实踩过：bilibili 下回来一个 9.4MB 的 mp3，程序报下载失败。）
    stdoutBuf.append(p.readAllStandardOutput());
    stderrBuf.append(p.readAllStandardError());

    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        res.error = humanizeError(QString::fromUtf8(stderrBuf), p.exitCode());
        return res;
    }

    // 定位输出文件，三级兜底（从最可靠到最宽松）：
    //   1) 我们自己算出来的目标路径 —— 输出模板是我们给的，零解析、最稳；
    //   2) --print after_move:filepath 打印的那行绝对路径 —— 覆盖 yt-dlp
    //      自行调整了扩展名的情况；
    //   3) 输出目录里 mtime 最新的音频文件 —— 前两级都失手时的安全网。
    // 为什么要三级：文件名里可能带 emoji / 特殊字符（实测过标题里带 🩵），
    // 只要有一处解码或拼接出偏差，"文件明明下好了却报失败"，
    // 而用户看到的是"下载失败" —— 这是最不能接受的一种误报。
    QString tpl = opt.outputTemplate;
    tpl.remove(QStringLiteral(".%(ext)s"));
    const QString expectPath = QDir(opt.outputDir).filePath(
        tpl + QLatin1Char('.') + opt.audioFormat);
    if (QFileInfo::exists(expectPath)) res.outputFile = QFileInfo(expectPath).absoluteFilePath();

    // --print after_move:filepath 会在最后输出一行绝对路径
    const QStringList tailLines = QString::fromUtf8(stdoutBuf)
                                      .split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                             Qt::SkipEmptyParts);
    if (res.outputFile.isEmpty()) {
        for (int i = tailLines.size() - 1; i >= 0; --i) {
            const QString cand = tailLines.at(i).trimmed();
            if (QFileInfo::exists(cand)) { res.outputFile = QFileInfo(cand).absoluteFilePath(); break; }
        }
    }
    if (res.outputFile.isEmpty() && QFileInfo::exists(lastDest))
        res.outputFile = QFileInfo(lastDest).absoluteFilePath();

    if (res.outputFile.isEmpty()) {
        // 第三级：扫输出目录，取 mtime 最新的音频文件
        const QDir dir(opt.outputDir);
        const QFileInfoList cands = dir.entryInfoList(
            { QStringLiteral("*.mp3"), QStringLiteral("*.flac"), QStringLiteral("*.m4a"),
              QStringLiteral("*.aac"), QStringLiteral("*.opus"), QStringLiteral("*.webm") },
            QDir::Files, QDir::Time);
        if (!cands.isEmpty()) {
            res.outputFile = cands.first().absoluteFilePath();
            qInfo() << "[yt-dlp] 用目录扫描兜底定位到输出文件:" << res.outputFile;
        }
    }

    res.ok = !res.outputFile.isEmpty();
    if (!res.ok) {
        // 到这一步是真没找到。把输出目录报出来，用户至少能自己去看一眼。
        res.error = QStringLiteral("下载结束但没找到输出文件（输出目录：%1）")
                        .arg(QDir::toNativeSeparators(opt.outputDir));
    }
    return res;
}

} // namespace ms
