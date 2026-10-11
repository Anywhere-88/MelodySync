// ============================================================================
//  src/main.cpp
//  MelodySync 程序入口。
//
//  ⚠ 合规声明（与本文件、README.md、UI 设置页三处一致，请勿删除）
//  ---------------------------------------------------------------------------
//  本软件仅限个人学习与研究使用，不得用于任何商业目的。
//  AI 全网搜仅聚合 CC 协议 / 公版 / 艺术家直供等合法免费源，以及各平台的官方
//  试听接口。不提供任何绕过 DRM 或平台会员限制的功能。用户应自行承担因下载
//  受版权保护内容而产生的全部法律责任。
// ============================================================================
#include "MainWindow.h"

#include "ai/IntentParser.h"
#include "audio/FFmpegTranscoder.h"
#include "auth/CloudSync.h"
#include "bridge/LocalAudioServer.h"
#include "core/HttpClient.h"
#include "data/Database.h"
#include "download/DownloadEngine.h"
#include "download/YtDlpWrapper.h"
#include "playlist/PlatformResolve.h"
#include "search/SearchAggregator.h"
#include "settings/Settings.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QMessageBox>
#include <QMutex>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QStringConverter>
#include <QTextStream>
#include <QTime>
#include <QTimer>

#include <cstdio>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

// 工程内所有类都放在 namespace ms 里（见 core/Types.h 的说明），但 main.cpp 本身
// 不属于 ms —— 它是程序入口，不该被塞进业务命名空间。这里用一条 using 指令把
// ms 里的名字引进来，避免满屏 ms:: 前缀。
//
// 注意：这条 using 只放在 .cpp 里，绝不能出现在任何头文件中，否则会把 ms 的
// 内容泄漏到包含它的所有翻译单元，引发难以定位的重名冲突。
using namespace ms;

namespace {

// 与 UI 设置页、README 保持一致的合规文案（一次定义，多处引用）
const char* kLegalNotice =
    "本软件仅限个人学习与研究使用，不得用于任何商业目的。"
    "AI 全网搜仅聚合 CC 协议 / 公版 / 艺术家直供等合法免费源，以及各平台的官方试听接口。"
    "不提供任何绕过 DRM 或平台会员限制的功能。"
    "用户应自行承担因下载受版权保护内容而产生的全部法律责任。";

// 允许 --dev 参数：加载磁盘上的 ui/index.html 而不是 qrc 内置版本
bool hasArg(int argc, char** argv, const char* flag)
{
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], flag) == 0) return true;
    return false;
}

// 取 --flag value 或 --flag=value 形式的值，没有则返回空串。
//
// 这里走 QCoreApplication::arguments() 而不是裸 argv：Windows 上 argv 是经过
// 系统 ANSI 代码页（中文机器上是 GBK）转换的，搜索词里一旦有 GBK 表示不了的
// 字符就会丢；arguments() 内部用的是宽字符命令行，能正确还原 Unicode。
QString argValue(const QString& flag)
{
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == flag && i + 1 < args.size()) return args.at(i + 1);
        if (args.at(i).startsWith(flag + QLatin1Char('=')))
            return args.at(i).mid(flag.size() + 1);
    }
    return QString();
}

// 把 qInfo/qWarning 同时写到日志文件。
// 路径由 QStandardPaths::AppDataLocation 决定，它在 Windows 上是
//     %APPDATA%\<组织名>\<应用名>\
// 而本工程的组织名与应用名都叫 MelodySync，所以是**两层同名目录**：
//     %APPDATA%\MelodySync\MelodySync\logs\melodysync.log
// （以前这里的注释少写了一层，照它去找会找不到文件。）
void installLogHandler()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QStringLiteral("/logs");
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/melodysync.log");

#ifdef Q_OS_WIN
    // 控制台切成 UTF-8。必须和下面给 stderr 写 UTF-8 字节配套，否则会乱码。
    // 放在这里（而不是只在 runProbe 里）是因为日志处理器是进程级安装的，
    // 任何一条 qInfo 都可能往 stderr 写。无控制台时调用失败，无害。
    SetConsoleOutputCP(65001);
#endif

    static QFile logFile(path);
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    static QTextStream stream(&logFile);
    // 显式锁定 UTF-8：QTextStream 的默认编码随 Qt 大版本变过（Qt 5 时代是
    // local8bit），写死才不会在某天升级后让整份日志变成乱码。
    stream.setEncoding(QStringConverter::Utf8);

    // QFile / QTextStream **都不是线程安全的**，而这里的日志调用来自线程池里的
    // 多个工作线程（搜索时 8 个源并行）。并发进入会踩坏 QTextStream 内部的写入
    // 缓冲与编码转换状态 —— 症状就是同一行中文字符串，这次写正常、下次写变成
    // 「UTF-8 字节被按 GBK 解读」的乱码（曾亲眼见到同一进程内 [YTMusic] 正常、
    // [SoundCloud] 乱码）。日志是排查问题的唯一手段，坏了代价很大，必须串行化。
    static QMutex logMutex;

    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
        const QString line = QStringLiteral("[%1] %2%3")
            .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                 msg,
                 ctx.file ? QStringLiteral("  (%1:%2)").arg(QString::fromUtf8(ctx.file)).arg(ctx.line)
                          : QString());

        {
            QMutexLocker locker(&logMutex);
            stream << line << Qt::endl;          // Qt::endl 自带 flush
            // stderr 也写 UTF-8，与日志文件严格同编码。
            // 以前这里用 toLocal8Bit()（简中＝GBK），于是
            // `--probe "歌名" > out.txt 2>&1` 落盘的文件里 GBK 与 UTF-8 混在一起：
            //   · 控制台/编辑器打开是半截乱码；
            //   · `grep` 判定为二进制，直接报 "Binary file matches"，根本没法检索。
            // 配套前提是上面 SetConsoleOutputCP(65001) —— 控制台已按 UTF-8 解读。
            fprintf(stderr, "%s\n", line.toUtf8().constData());
        }

        if (type == QtFatalMsg) abort();
    });
}

} // namespace

// ===========================================================================
//  --probe：命令行探针
//
//  为什么要有它：GUI 里"搜不到"和"下不动"是混在一起的一团 ——
//  8 个源里哪个挂了、平台为什么拒绝这首、yt-dlp 是不是坏了，界面上都看不清。
//  这条链路能不能通，用一个命令就能问明白。
//
//  它做的事和 GUI 完全一样（同一套 SearchAggregator / PlatformResolve /
//  DownloadEngine），只是不开窗口。
//
//  注意：本程序是 /subsystem:windows 构建，printf 默认没有控制台可写，
//  所以这里先 AttachConsole 抢回父进程的控制台；抢不到也不要紧 ——
//  所有输出同时走 qInfo，日志文件里一定有完整记录（末尾会打印日志路径）。
// ===========================================================================
namespace {

void probeSay(const QString& line)
{
    // 两条腿走路：终端能看到，日志里也留一份。
    //
    // 编码必须是 UTF-8，不能图省事用 toLocal8Bit()：runProbe 开头已经
    // SetConsoleOutputCP(65001) 把控制台切成 UTF-8 了，而 toLocal8Bit() 给的是
    // **系统 ANSI 代码页**（简中＝GBK）字节 —— 喂给 UTF-8 控制台就是乱码。
    // 重定向到文件时同样必须与日志同编码（日志写的是 UTF-8），否则同一个文件里
    // 两种编码混在一起，连 grep 和编辑器都会把它当二进制
    // （实测 `grep probe w2.txt` → "Binary file w2.txt matches"）。
    fprintf(stdout, "%s\n", line.toUtf8().constData());
    fflush(stdout);
    qInfo().noquote() << QStringLiteral("[probe]") << line;
}

int runProbe(const QString& query, const QString& downloadCountArg,
             const QString& fallbackCountArg, const QString& songArg)
{
#ifdef Q_OS_WIN
    // 抢父控制台 + 切 UTF-8 代码页，中文才不会变成乱码。
    //
    // ⚠ 只在 stdout **没有被重定向**时才抢。
    //   `freopen_s(&dummy, "CONOUT$", "w", stdout)` 会把调用方写的
    //   `--probe > out.txt` 整个丢掉：之后所有探针输出都直奔控制台，
    //   落盘文件里只剩下启动那十几行。而探针的主输出通道就是 stdout ——
    //   tools\probe_regression.py 那种"读日志"的调用方完全看不出问题，
    //   但人手动重定向存证时会以为探针根本没跑（实测踩过：文件只有 15 行）。
    //   判据用 GetFileType：重定向到文件是 FILE_TYPE_DISK，管道是 FILE_TYPE_PIPE，
    //   真控制台是 FILE_TYPE_CHAR —— 只有后者才需要抢。
    const DWORD outType = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    const bool redirected = (outType == FILE_TYPE_DISK || outType == FILE_TYPE_PIPE);
    if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(65001);
#endif

    if (query.trimmed().isEmpty()) {
        probeSay(QStringLiteral(
            "用法：MelodySync.exe --probe \"歌名 歌手\" [--probe-download N] [--probe-fallback N] [--probe-song \"歌手|歌名|时长秒\"]"));
        probeSay(QStringLiteral(
            "  --probe-download N  真的下载前 N 条**有直链**的结果"));
        probeSay(QStringLiteral(
            "  --probe-fallback N  对前 N 条**没有直链**（仅试听/会员曲）的结果走一次"
            "「换源下载」，验证候选打分选优链路"));
        probeSay(QStringLiteral(
            "  --probe-song \"歌手|歌名|时长秒\"  直接对这条曲目走一次「换源下载」"
            "（复现歌单场景：字段来自平台曲库，歌手可能是多歌手串）"));
        return 2;
    }

    Settings& cfg = Settings::instance();

    SearchAggregator agg;
    SearchAggregator::Config c;
    c.enabledSources  = cfg.enabledSources();
    c.limitPerSource  = 12;
    c.maxParallel     = 8;
    agg.setConfig(c);

    probeSay(QStringLiteral("=== 启用的搜索源（%1 个）===").arg(c.enabledSources.size()));
    probeSay(QStringLiteral("  %1").arg(c.enabledSources.join(QStringLiteral(", "))));
    // 逐个报可用性：缺 client_id / yt-dlp 损坏都会在这里现形
    for (const SearchAggregator::SourceStatus& st : agg.sourceStatus()) {
        probeSay(QStringLiteral("  %1  %2%3")
                     .arg(st.id, -11)
                     .arg(st.available ? QStringLiteral("可用") : QStringLiteral("不可用"))
                     .arg(st.reason.isEmpty() ? QString()
                                              : QStringLiteral("  — ") + st.reason));
    }

    IntentParser parser;
    QString engine, llmError;
    const SearchIntent intent = parser.parse(query, &engine, &llmError);
    probeSay(QStringLiteral("=== 意图解析 ==="));
    probeSay(QStringLiteral("  引擎=%1  歌名=%2  歌手=%3  检索词=%4")
                 .arg(engine, intent.name.isEmpty() ? QStringLiteral("(无)") : intent.name,
                      intent.artist.isEmpty() ? QStringLiteral("(无)") : intent.artist,
                      intent.searchText()));
    if (!intent.altQueries.isEmpty()) {
        probeSay(QStringLiteral("  原语言检索词: %1").arg(intent.altQueries.join(QStringLiteral(" / "))));
        probeSay(QStringLiteral("  原语言歌名:   %1  歌手: %2")
                     .arg(intent.altNames.join(QStringLiteral(" / ")),
                          intent.altArtist.isEmpty() ? QStringLiteral("(无)") : intent.altArtist));
    }
    if (!llmError.isEmpty())
        probeSay(QStringLiteral("  ⚠ AI 未生效，已回落规则解析：%1").arg(llmError));

    probeSay(QStringLiteral("=== 开始搜索 ==="));
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    const SearchAggregator::Result res = agg.searchAll(intent);
    probeSay(QStringLiteral("  耗时 %1 ms，原始 %2 条 → 去重后 %3 条")
                 .arg(QDateTime::currentMSecsSinceEpoch() - t0)
                 .arg(res.rawCount).arg(res.hits.size()));

    // 按源汇总"命中多少 / 其中多少条真能下"
    QMap<QString, QPair<int, int>> perSrc;
    for (const SearchHit& h : res.hits) {
        auto& v = perSrc[h.sourceId];
        ++v.first;
        if (!h.downloadUrl.isEmpty()) ++v.second;
    }
    probeSay(QStringLiteral("=== 各源命中（命中 / 可下载）==="));
    for (auto it = perSrc.constBegin(); it != perSrc.constEnd(); ++it)
        probeSay(QStringLiteral("  %1  %2 / %3").arg(it.key(), -11)
                     .arg(it.value().first).arg(it.value().second));

    const int dlTotal = std::count_if(res.hits.cbegin(), res.hits.cend(),
                                      [](const SearchHit& h) { return !h.downloadUrl.isEmpty(); });
    probeSay(QStringLiteral("=== 结果清单（可下载 %1 / 共 %2）===").arg(dlTotal).arg(res.hits.size()));
    int shown = 0;
    for (const SearchHit& h : res.hits) {
        if (shown++ >= 30) { probeSay(QStringLiteral("  …（只列前 30 条）")); break; }
        probeSay(QStringLiteral("  [%1] %2 | %3 — %4 | %5 | %6 | %7 MB | 匹配%8 | %9 | %10")
                     .arg(shown).arg(h.sourceId, -10).arg(h.title, h.artist)
                     .arg(qualityLabel(h.quality))
                     .arg(h.durationText())
                     .arg(h.fileSizeKb > 0 ? QString::number(h.fileSizeKb / 1024.0, 'f', 1)
                                           : QStringLiteral("?"))
                     .arg(QString::number(h.score, 'f', 0))
                     .arg(h.heatText.isEmpty() ? QStringLiteral("热度?") : h.heatText)
                     .arg(h.downloadUrl.isEmpty()
                              ? QStringLiteral("仅试听：%1").arg(h.note)
                              : QStringLiteral("可下载")));
        // 评分拆解：调权重时全靠它看清每一项的贡献（见 IntentParser.cpp 文件头）
        probeSay(QStringLiteral("        评分拆解: %1").arg(h.scoreDetail));
    }

    // ---- 真下载，验证落盘 ----
    const int want = downloadCountArg.toInt();
    const int fallbackWant = fallbackCountArg.toInt();
    if (want <= 0 && fallbackWant <= 0 && songArg.trimmed().isEmpty()) {
        probeSay(QStringLiteral("（加 --probe-download N 可以真的下载前 N 条以验证链路）"));
        probeSay(QStringLiteral("（加 --probe-fallback N 可以验证「换源下载」的候选打分选优）"));
        return 0;
    }

    // 落到临时目录，别把用户的音乐库当试验田
    const QString probeDir = QDir::tempPath() + QStringLiteral("/melodysync-probe");
    QDir().mkpath(probeDir);
    const QString savedDir = cfg.downloadDir();
    cfg.setDownloadDir(probeDir);
    probeSay(QStringLiteral("=== 试下载（落盘到 %1）===").arg(probeDir));

    DownloadEngine dl;
    int done = 0, failed = 0;

    auto runOne = [&](const SearchHit& h, bool useDirectUrl) {
        DownloadTask t;
        t.id         = h.id;
        t.platformId = PlatformResolve::platformIdOf(h.id);
        t.title      = h.title;
        t.artist     = h.artist;
        t.album      = h.album;
        t.sourceId   = h.sourceId;
        t.quality    = h.quality;
        t.durationSec = h.durationSec;      // 换源候选打分要拿它核对时长
        // 刻意留空 url 就会走"换源下载"（见 DownloadEngine 的 forceSearchFallback
        // 说明）：平台直链拿不到时，用"歌手 + 歌名"去全网搜候选，再按相关度选优。
        t.url        = useDirectUrl ? h.downloadUrl : QString();
        t.coverUrl   = h.coverUrl;
        t.searchText = (h.artist + QLatin1Char(' ') + h.title).trimmed();

        const DownloadEngine::Outcome out =
            dl.downloadOne(t, [](int, qint64) {}, CancelTokenPtr());
        if (out.ok) {
            ++done;
            probeSay(QStringLiteral("  ✓ %1 — %2（%3 字节）")
                         .arg(t.title, out.filePath,
                              QString::number(QFileInfo(out.filePath).size())));
        } else {
            ++failed;
            probeSay(QStringLiteral("  ✗ %1 — %2").arg(t.title, out.error));
        }
    };

    // A) 有直链的：直接下，验证"直链 + 落盘"这段
    if (want > 0) {
        probeSay(QStringLiteral("--- A) 直链下载（前 %1 条可下载结果）---").arg(want));
        int n = 0;
        for (const SearchHit& h : res.hits) {
            if (n >= want) break;
            if (h.downloadUrl.isEmpty()) continue;
            ++n;
            probeSay(QStringLiteral("  · 目标 %1: %2 — %3 | %4 | 原曲时长 %5")
                         .arg(n).arg(h.title, h.artist, h.sourceId, h.durationText()));
            runOne(h, /*useDirectUrl=*/true);
        }
    }

    // B) 没有直链的（仅试听 / 会员曲）：走「换源下载」，验证候选打分选优。
    //    这是用户最容易踩的路径 —— 以前换源是直接抓搜索结果第一条，
    //    撞上翻唱/现场/试音视频就"货不对版本"，反馈过好几次。
    if (fallbackWant > 0) {
        probeSay(QStringLiteral("--- B) 换源下载（前 %1 条仅试听结果）---").arg(fallbackWant));
        int n = 0;
        for (const SearchHit& h : res.hits) {
            if (n >= fallbackWant) break;
            if (!h.downloadUrl.isEmpty()) continue;
            // 全网源的命中本来就是页面地址，没有"平台直链"可谈，换源对它无意义
            if (h.sourceId == QStringLiteral("web")) continue;
            ++n;
            probeSay(QStringLiteral("  · 目标 %1: %2 — %3 | %4 | %5 | 原曲时长 %6")
                         .arg(n).arg(h.title, h.artist, h.sourceId, h.note, h.durationText()));
            runOne(h, /*useDirectUrl=*/false);
        }
    }

    // C) 直接指定一条曲目走换源：复现"歌单里的歌换源下错版本"。
    //    歌单任务不是从搜索结果来的，字段直接来自平台曲库 —— 最典型的是 artist
    //    为多歌手串（"Cartoon / Jéja / Coleman Trapp"）。--probe-fallback 覆盖不到
    //    这种场景（它只能挑搜索结果里恰好没有直链的条目）。
    if (!songArg.trimmed().isEmpty()) {
        const QStringList f = songArg.split(QLatin1Char('|'));
        if (f.size() < 2) {
            probeSay(QStringLiteral("  ✗ --probe-song 需要 \"歌手|歌名|时长秒\"，实际收到：%1")
                         .arg(songArg));
        } else {
            SearchHit h;
            h.id          = QStringLiteral("probe:song");
            h.sourceId    = QStringLiteral("netease");
            h.artist      = f.value(0).trimmed();
            h.title       = f.value(1).trimmed();
            h.durationSec = f.value(2).trimmed().toInt();
            h.quality     = Quality::K320;
            h.note        = QStringLiteral("--probe-song 指定");
            probeSay(QStringLiteral("--- C) 指定曲目换源：%1 — %2 | 原曲时长 %3 秒 ---")
                         .arg(h.title, h.artist).arg(h.durationSec));
            runOne(h, /*useDirectUrl=*/false);
        }
    }

    cfg.setDownloadDir(savedDir);
    probeSay(QStringLiteral("=== 试下载结果：成功 %1，失败 %2 ===").arg(done).arg(failed));

    probeSay(QStringLiteral("日志：%1")
                 .arg(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                      + QStringLiteral("/logs/melodysync.log")));
    return (done > 0) ? 0 : 1;
}

// ===========================================================================
//  --probe-cloud <apiBase>：云端账户端到端自检
//
//  为什么要有它：账号链路（注册/登录/心跳/歌单同步）藏在 GUI 个人中心里，
//  部署完云函数手点 UI 验证既慢又说不清哪步断了。一条命令走完全部动作，
//  每步 PASS/FAIL 直接打在 stdout —— 也能对着本地 mock 服务器测客户端传输层。
//
//  动作序列（每步最多等总超时兜底）：
//    1) register（重名则自动改走 login）
//    2) heartbeat（验证 token 会话）
//    3) pushPlaylist（整包推）
//    4) pullPlaylist（拉回并核对内容）
//    5) 错误密码登录（应被拒绝 —— 安全验证）
//  全部通过返回 0，任何一步失败返回 1。
// ===========================================================================
int runCloudProbe(const QString& apiBase)
{
#ifdef Q_OS_WIN
    // 与 runProbe 相同的控制台处理：只在 stdout 未重定向时抢父控制台
    const DWORD outType = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    const bool redirected = (outType == FILE_TYPE_DISK || outType == FILE_TYPE_PIPE);
    if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen_s(nullptr, "CONOUT$", "w", stdout);
        freopen_s(nullptr, "CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(65001);
#endif
    if (apiBase.trimmed().isEmpty()) {
        probeSay(QStringLiteral("用法：MelodySync.exe --probe-cloud <云函数HTTP访问地址>"));
        return 2;
    }

    CloudSync cloud;
    cloud.configureFromSettings(apiBase);

    const QString user  = QStringLiteral("probe_user");
    const QString pwd   = QStringLiteral("probe_pwd_hash_0123456789");
    const QString items = QStringLiteral("[{\"name\":\"probe-checklist\",\"songs\":[1,2,3]}]");

    QEventLoop loop;
    QTimer::singleShot(90 * 1000, &loop, [&loop]() { loop.exit(2); });   // 总超时兜底

    QString expectedAction;          // 当前等待的 syncSucceeded/syncFailed action
    QString pullItems;
    bool    stepOk = false;
    QString stepErr;
    int     failures = 0;

    auto failStep = [&](const QString& name, const QString& why) {
        ++failures;
        probeSay(QStringLiteral("  ✗ %1: %2").arg(name, why));
    };

    QObject::connect(&cloud, &CloudSync::syncSucceeded,
                     [&](const QString& action, const QString&) {
                         if (action == expectedAction) { stepOk = true; loop.quit(); }
                     });
    QObject::connect(&cloud, &CloudSync::syncFailed,
                     [&](const QString& action, const QString& err) {
                         if (action == expectedAction) { stepOk = false; stepErr = err; loop.quit(); }
                     });
    QObject::connect(&cloud, &CloudSync::playlistPulled, [&](const QString& pulled) {
        if (expectedAction == QStringLiteral("pull")) {
            pullItems = pulled;
            stepOk = true;
            loop.quit();
        }
    });

    auto waitStep = [&]() {
        stepOk = false;
        stepErr.clear();
        loop.exec();
    };

    // 1) 注册（重名 → 自动改走登录，也算过）
    expectedAction = QStringLiteral("register");
    probeSay(QStringLiteral("--- 1) register ---"));
    cloud.reportRegister(user, pwd, QStringLiteral("🎵"));
    waitStep();
    if (stepOk) {
        probeSay(QStringLiteral("  ✓ PASS"));
    } else {
        probeSay(QStringLiteral("  · register 未通过（%1），改走 login…").arg(stepErr));
        expectedAction = QStringLiteral("login");
        cloud.reportLogin(user, pwd);
        waitStep();
        if (stepOk) probeSay(QStringLiteral("  ✓ login PASS（账户已存在）"));
        else failStep(QStringLiteral("register+login"), stepErr);
    }

    // 2) 心跳（验证 token 会话有效）
    if (failures == 0) {
        expectedAction = QStringLiteral("heartbeat");
        probeSay(QStringLiteral("--- 2) heartbeat ---"));
        cloud.heartbeat();
        waitStep();
        if (stepOk) probeSay(QStringLiteral("  ✓ PASS（在线状态已刷新）"));
        else failStep(QStringLiteral("heartbeat"), stepErr);
    }

    // 3) 歌单整包推
    if (failures == 0) {
        expectedAction = QStringLiteral("pushPlaylist");
        probeSay(QStringLiteral("--- 3) pushPlaylist ---"));
        cloud.pushPlaylist(user, items);
        waitStep();
        if (stepOk) probeSay(QStringLiteral("  ✓ PASS"));
        else failStep(QStringLiteral("pushPlaylist"), stepErr);
    }

    // 4) 拉回并核对内容
    if (failures == 0) {
        expectedAction = QStringLiteral("pull");
        probeSay(QStringLiteral("--- 4) pullPlaylist ---"));
        cloud.pullPlaylist(user);
        waitStep();
        if (stepOk && pullItems.contains(QLatin1String("probe-checklist")))
            probeSay(QStringLiteral("  ✓ PASS（内容一致，%1 字节）").arg(pullItems.size()));
        else failStep(QStringLiteral("pullPlaylist"),
                      pullItems.isEmpty() ? QStringLiteral("云端无歌单") : QStringLiteral("内容不一致"));
    }

    // 5) 错误密码应被拒绝（cloudLoginResult 路径，WebBridge 跨设备登录用的就是它）。
    //    ⚠ 前置步骤全过才跑：网络不通时 cloudLoginResult(false) 无法区分
    //    「服务端拒绝」与「连接失败」，误报 PASS 会把真问题盖住。
    if (failures == 0) {
        CloudSync cloud2;
        cloud2.configureFromSettings(apiBase);
        probeSay(QStringLiteral("--- 5) 错误密码登录（应拒绝）---"));
        bool rejected = false;
        QObject::connect(&cloud2, &CloudSync::cloudLoginResult,
                         [&](bool ok, const QString&, const QString&,
                             const QString&, const QString&) {
                             rejected = !ok;
                             loop.quit();
                         });
        cloud2.cloudLogin(user, QStringLiteral("wrong_password_hash"));
        loop.exec();
        if (rejected) probeSay(QStringLiteral("  ✓ PASS（已拒绝）"));
        else { ++failures; probeSay(QStringLiteral("  ✗ 错误密码竟然登录成功 —— 服务端校验有问题！")); }
    } else {
        probeSay(QStringLiteral("--- 5) 错误密码登录：跳过（前置步骤有失败，结果不可信）---"));
    }

    probeSay(QStringLiteral("=== CloudProbe：%1 ===")
                 .arg(failures == 0 ? QStringLiteral("全部通过")
                                    : QStringLiteral("%1 项失败").arg(failures)));
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    // Chromium 参数必须在 QApplication 之前设置。
    // 注意：曾经开过 EnableTransparentVisuals（让圆角/阴影不显示灰边），但它会让
    // WebEngine 把整个页面当成“透明层”去和桌面合成——玻璃面板(backdrop-filter)
    // 因此持续对桌面采样，表现就是“一直闪”。如今默认全屏、本就看不見壁纸，玻璃直接
    // 对页面深色底模糊即可，这个特性纯属负担，故关闭。（见 MainWindow 的闪屏修复）
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
            qgetenv("QTWEBENGINE_CHROMIUM_FLAGS")
            + QByteArray(" --disable-features=CalculateNativeWinOcclusion"));

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MelodySync"));
    QCoreApplication::setApplicationName(QStringLiteral("MelodySync"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    // 唯一还会出现的原生控件是 QMessageBox（数据库初始化失败时），
    // 不样式化就是一块刺眼的 Windows 默认白框 —— 全部按前端深色主题染色。
    app.setStyleSheet(QStringLiteral(
        "QMessageBox{background:#12151c;}"
        "QMessageBox QLabel{color:#f0f2f7;font-size:13px;}"
        "QMessageBox QPushButton{background:#1f242f;color:#f0f2f7;"
        "  border:1px solid rgba(255,255,255,.14);border-radius:6px;"
        "  padding:5px 16px;min-width:64px;}"
        "QMessageBox QPushButton:hover{background:#2a303d;}"));

    installLogHandler();

    qInfo() << "============================================================";
    qInfo() << " MelodySync 1.0.0 启动";
    qInfo() << " Qt" << QT_VERSION_STR << "| 前端: QWebEngine + QWebChannel";
    qInfo() << " " << QString::fromUtf8(kLegalNotice);
    qInfo() << "============================================================";

    // ---- 全局子系统初始化（顺序有讲究）----
    HttpClient::init();          // libcurl 全局初始化，必须早于任何网络调用
    FFmpegTranscoder::init();    // 设置 FFmpeg 日志级别
    // 本地音频 HTTP 服务（127.0.0.1）：前端 <audio> 播放本地音乐库文件
    // 的唯一通路 —— file:// 会被 Chromium 当跨源子资源拦截（实测）。
    LocalAudioServer::instance().start();

    if (!Database::instance().open()) {
        QMessageBox::critical(
            nullptr,
            QStringLiteral("MelodySync 启动失败"),
            QStringLiteral("无法初始化本地数据库：\n%1\n\n请检查 %2 是否有写入权限。")
                .arg(Database::instance().lastError(),
                     QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)));
        return 1;
    }
    qInfo() << "[MelodySync] 数据库:" << Database::instance().path();

    // 设置默认值（首次运行会写入 settings.ini）
    Settings& cfg = Settings::instance();
    cfg.migrate();                 // 一次性配置迁移（新增搜索源等），必须在任何读配置之前
    QDir().mkpath(cfg.downloadDir());
    qInfo() << "[MelodySync] 配置文件:" << cfg.filePath();
    qInfo() << "[MelodySync] 下载目录:" << cfg.downloadDir();

    // 把 ini 里的自定义 yt-dlp 路径灌给下载器。
    // 必须在任何 YtDlpWrapper 实例被构造之前调用 —— 构造函数里就会做一次
    // executablePath() 解析并把结果缓存到 m_exe，之后再改静态路径对已存在的
    // 实例无效。空字符串是"没配过"，此时 YtDlpWrapper 会走六段式候选查找
    // （exe 同级 bin\ → exe 同级 → ..\bin\ → ~/.melodysync/bin → PATH）。
    YtDlpWrapper::setCustomPath(cfg.ytDlpPath());
    qInfo() << "[MelodySync] yt-dlp:" << YtDlpWrapper::executablePath();

    // --probe：不开窗口的命令行探针，把"搜索 → 取直链 → 真下载"整条链路跑一遍，
    // 结果按行打到 stdout。存在的意义很实际：
    //   GUI 里出问题时，"搜不到"和"下不动"混在一起很难分辨是哪个源、哪一步坏了；
    //   而这条链路能不能通，用一个命令就能问清楚。
    // 用法：
    //   MelodySync.exe --probe "歌名 歌手"              只搜索，列出每个源的命中与可下载数
    //   MelodySync.exe --probe "歌名" --probe-download 1 额外真下载第 1 条可下载结果
    //   MelodySync.exe --probe "歌名" --probe-fallback 1 对第 1 条"仅试听"结果走换源下载
    //   MelodySync.exe --probe "歌名" --probe-song "歌手|歌名|时长秒" 直接对指定曲目走换源
    //
    // 注意：--probe 本身是必需的（它带搜索词），--probe-song 只是它的附加开关。
    // 这几个开关都用 hasArg() 精确匹配，所以漏写 --probe 时下面这个 if 不成立、
    // 程序会**静默去开 GUI** —— 命令行探针变成弹窗口，很容易被误判成"探针没反应"。
    // 因此这里显式拦一下并给出提示。
    const bool probeGuestFlag = hasArg(argc, argv, "--probe-song")
                                || hasArg(argc, argv, "--probe-download")
                                || hasArg(argc, argv, "--probe-fallback");
    if (!hasArg(argc, argv, "--probe") && probeGuestFlag) {
        qWarning().noquote()
            << QStringLiteral("提示：--probe-song / --probe-download / --probe-fallback 是 "
                              "--probe 的附加开关，必须同时给出 --probe \"歌名 歌手\"。");
        qWarning().noquote()
            << QStringLiteral("例：MelodySync.exe --probe \"Why We Lose\" "
                              "--probe-song \"Cartoon / Jéja / Coleman Trapp|Why We Lose|197\"");
        return 2;
    }
    if (hasArg(argc, argv, "--probe-cloud"))
        return runCloudProbe(argValue(QStringLiteral("--probe-cloud")));
    if (hasArg(argc, argv, "--probe"))
        return runProbe(argValue(QStringLiteral("--probe")),
                        argValue(QStringLiteral("--probe-download")),
                        argValue(QStringLiteral("--probe-fallback")),
                        argValue(QStringLiteral("--probe-song")));

    MainWindow window;
    window.loadFrontend(hasArg(argc, argv, "--dev"));

    // -----------------------------------------------------------------------
    // 单实例守护：普通 GUI 启动时，若已有实例在跑，就把已有窗口拉到最前，
    // 然后自己退出。实测双击两次快捷方式会起两个全屏窗口 —— 共用同一份
    // SQLite 数据库和下载队列，互相踩不说，用户还以为程序出了 bug。
    //
    // ⚠ --probe / --selftest 必须绕过守护：它们是无头/自动化验证，
    //   设计上就允许与用户正在使用的实例并存。
    // -----------------------------------------------------------------------
    if (!hasArg(argc, argv, "--probe") && !hasArg(argc, argv, "--selftest")) {
        static const char* kGuardKey = "MelodySync.SingleInstance.Guard";
        QLocalSocket ping;
        ping.connectToServer(QLatin1String(kGuardKey));
        if (ping.waitForConnected(300)) {
            ping.write("activate\n");
            ping.waitForBytesWritten(300);
            qInfo() << "[MelodySync] 已有实例在运行，激活其窗口后退出";
            return 0;
        }
        QLocalServer::removeServer(QLatin1String(kGuardKey));   // 清理崩溃残留
        QLocalServer* guard = new QLocalServer(&app);
        guard->listen(QLatin1String(kGuardKey));
        QObject::connect(guard, &QLocalServer::newConnection, [&window, guard]() {
            while (QLocalSocket* c = guard->nextPendingConnection()) {
                c->readAll();
                c->disconnectFromServer();
                c->deleteLater();
            }
            // 最小化/被遮挡时双击图标，把已有全屏窗口找回来
            window.setWindowState(window.windowState() & ~Qt::WindowMinimized);
            window.showFullScreen();
            window.raise();
            window.activateWindow();
        });
    }

    // --selftest：等页面就绪后在 JS 里把每个桥接方法跑一遍，结果写进日志。
    // 前端的「调方法 + 等信号」契约只有真正在页面里执行一次才验证得了
    // （信号名对不上、返回值忘 await 这类问题静态检查看不出来）。
    const bool selfTest = hasArg(argc, argv, "--selftest");

    if (selfTest) {
        // 自检模式必须**真的显示窗口**：Chromium 会把不可见页面的 JS 挂起，
        // 隐藏运行时 runJavaScript 的回调永远不触发（实测：连同步脚本都没反应）。
        // 但某些受限会话又会在窗口出现后立刻发 WM_CLOSE —— 所以一边显示，
        // 一边让窗口拒绝关闭，直到自检跑完由我们自己退出。
        window.setIgnoreClose(true);
        window.showFullScreen();
        qInfo() << "[Selftest] 已显示窗口（并拒绝外部关闭请求）";
        window.runSelfTest(argValue(QStringLiteral("--selftest-query")),
                           argValue(QStringLiteral("--selftest-playlist")));
        // 自检脚本自身有 8s 延迟，再加上搜索/解析的网络耗时，留足余量；
        // 无论成败都退出，由调用方去日志里判定结果。
        QTimer::singleShot(150000, &app, []() { qApp->quit(); });
    } else {
        // 默认进入全屏：启动即铺满屏幕（F11 可在全屏 / 窗口之间切换）
        window.showFullScreen();
    }

    const int rc = app.exec();

    Database::instance().close();
    HttpClient::shutdown();
    qInfo() << "[MelodySync] 正常退出，返回码" << rc;
    return rc;
}
