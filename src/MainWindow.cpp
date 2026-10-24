// ============================================================================
//  src/MainWindow.cpp
// ============================================================================
#include "MainWindow.h"

#include "bridge/WebBridge.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPalette>
#include <QShortcut>
#include <QTimer>
#include <QVariant>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <memory>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace ms {

// ---------------------------------------------------------------------------
// 把前端 console.log 转发到 Qt 日志，方便排查（否则页面里报错是静默的）。
//
// 注意 QWebEnginePage::javaScriptConsoleMessage 是 **protected 虚函数**，
// 不是信号 —— 既不能 connect，也不能对它取成员函数指针。唯一正确的做法是
// 子类化并 override。（之前写成 connect(...) 直接编译失败：
//    error C2248: "QWebEnginePage::javaScriptConsoleMessage":
//    无法访问 protected 成员）
//
// 这里不定义信号槽，所以不需要 Q_OBJECT，也就不牵扯 moc。
// ---------------------------------------------------------------------------
namespace {
class ConsoleForwardingPage : public QWebEnginePage
{
public:
    explicit ConsoleForwardingPage(QWebEngineProfile* profile, QObject* parent = nullptr)
        : QWebEnginePage(profile, parent) {}

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                  const QString& message,
                                  int lineNumber,
                                  const QString& sourceID) override
    {
        Q_UNUSED(level)
        qInfo().noquote() << QStringLiteral("[JS %1:%2] %3")
                                 .arg(sourceID).arg(lineNumber).arg(message);
    }
};
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("MelodySync — 歌单导入 · AI 全网搜 · 无损下载 · 设备同步"));
    setMinimumSize(1180, 760);
    resize(1440, 900);

    applyDarkPalette();
    setupWebEngine();
    setupBridge();
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
void MainWindow::applyDarkPalette()
{
    // WebEngine 在页面加载完之前会先绘制一个白底，深色界面下会"闪一下白"。
    // 把窗口与视口调色板设为深色可以显著减轻这一瞬的刺眼感。
    //
    // 2026-10-24：界面默认主题换成「暖纸编辑部」纸面（Settings v5 迁移），
    // 调色板同步改为纸面底色，避免启动首帧从深色跳到浅色的"闪黑"。
    QPalette p = palette();
    p.setColor(QPalette::Window, QColor(0xfa, 0xf7, 0xf2));
    p.setColor(QPalette::Base, QColor(0xfa, 0xf7, 0xf2));
    p.setColor(QPalette::WindowText, QColor(0x1f, 0x1d, 0x1a));
    setPalette(p);
    setAutoFillBackground(true);
}

// ---------------------------------------------------------------------------
void MainWindow::setupWebEngine()
{
    m_view = new QWebEngineView(this);
    // 关键：把原生控件本身也刷成深色不透明。WebEngine 在页面首帧画出之前会先露
    // 出视口底色，不处理就会“白闪”一下；这里让视口一出生就是深色，配合下面
    // setBackgroundColor 与 EnableTransparentVisuals 关闭，彻底消灭启动白闪。
    //
    // 2026-09-23 改：原来是 setStyleSheet("background-color:#0d0f14;")，改成
    // 调色板 + setAutoFillBackground。给 QWebEngineView 挂样式表会让它走
    // QStyleSheetStyle 并打开 WA_StyledBackground，于是每次自绘都要用样式表刷
    // 一遍背景盒；而这个控件整块都被 WebEngine 的合成表面盖住，父级的背景填充
    // 和子级画面交替出现，观感就是整屏“忽明忽暗”——闲置时不出新帧所以看不见，
    // 鼠标一动/一滚动/一点按就闪。用调色板染色同样能兜住首帧之前的底色，
    // 但只在尺寸/显示状态变化时才填一次，不会每帧参与。
    m_view->setAttribute(Qt::WA_TranslucentBackground, false);
    m_view->setAutoFillBackground(true);
    {
        QPalette vp = m_view->palette();
        // 与 applyDarkPalette 同步：默认主题已是「暖纸」纸面，视口底色跟着走
        vp.setColor(QPalette::Window, QColor(0xfa, 0xf7, 0xf2));
        vp.setColor(QPalette::Base, QColor(0xfa, 0xf7, 0xf2));
        m_view->setPalette(vp);
    }
    setCentralWidget(m_view);

    // 独立的 Profile：把缓存放在程序自己的数据目录，不污染系统默认浏览器
    auto* profile = new QWebEngineProfile(QStringLiteral("melodysync"), this);
    profile->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
    profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);

    // 用子类替换裸 QWebEnginePage —— 它负责把 JS 控制台输出转发到 Qt 日志
    m_page = new ConsoleForwardingPage(profile, this);
    m_view->setPage(m_page);

    // ---- 消灭一切"Windows 默认白框"之一：页面底色 ----
    // WebEngine 页面加载完成前（以及主题切换瞬间）的默认底色是白的，
    // 深色应用每次启动都会白屏闪一下。把页面底色设成前端的 --bg-1。
    m_page->setBackgroundColor(QColor(0x0d, 0x0f, 0x14));

    // ---- 消灭之二：原生右键菜单 ----
    // QWebEngineView 默认右键弹 Windows 原生菜单（白底），与应用的深色
    // 界面格格不入。本应用没有任何需要右键的场景，直接禁掉 —— 前端
    // 页面自身不注册 contextmenu 事件，行为等于"右键无反应"。
    m_view->setContextMenuPolicy(Qt::NoContextMenu);

    // ---- 消灭之三：原生白标题栏 ----
    // DWM 的"沉浸式深色模式"开关（Win10 1903+ / Win11）。两个属性号都试：
    // 20 是公开的 DWMWA_USE_IMMERSIVE_DARK_MODE，19 是部分旧版本使用的值。
    // 失败静默忽略 —— 老系统上顶多保持原样，不值得为此弹任何提示。
    // 顺带把 Win11 的标题栏底色和窗口边框也直接染成深色（属性 35/34）。
#ifdef Q_OS_WIN
    {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
        const COLORREF caption = RGB(0x0d, 0x0f, 0x14);
        DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));  // DWMWA_CAPTION_COLOR
        DwmSetWindowAttribute(hwnd, 34, &caption, sizeof(caption));  // DWMWA_BORDER_COLOR
    }
#endif

    QWebEngineSettings* s = m_page->settings();
    // 前端要读本地音频、发跨域请求（各搜索源 API），这些开关必须打开
    s->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    s->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    s->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    s->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, true);
    s->setAttribute(QWebEngineSettings::ScrollAnimatorEnabled, true);
    s->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    s->setAttribute(QWebEngineSettings::ShowScrollBars, true);

    // ---- 关键：在所有页面脚本之前注入 qwebchannel.js ----
    QFile f(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
    if (f.open(QIODevice::ReadOnly)) {
        QWebEngineScript script;
        script.setName(QStringLiteral("qwebchannel.js"));
        script.setSourceCode(QString::fromUtf8(f.readAll()));
        script.setInjectionPoint(QWebEngineScript::DocumentCreation);
        script.setWorldId(QWebEngineScript::MainWorld);
        script.setRunsOnSubFrames(false);
        m_page->scripts().insert(script);
        f.close();
        qInfo() << "[Window] 已注入 qwebchannel.js";
    } else {
        qWarning() << "[Window] 找不到 :/qtwebchannel/qwebchannel.js，"
                      "前端将回落到浏览器 mock 模式";
    }

    // 控制台转发已经在 ConsoleForwardingPage::javaScriptConsoleMessage 里
    // 通过 override 完成，这里不再需要（也无法）用 connect 去接一个
    // protected 虚函数。
}

// ---------------------------------------------------------------------------
void MainWindow::setupBridge()
{
    m_bridge = new WebBridge(this);
    m_bridge->bootstrap();

    auto* channel = new QWebChannel(this);
    // 名字必须与前端 ch.objects.bridge 一致
    channel->registerObject(QStringLiteral("bridge"), m_bridge);
    m_page->setWebChannel(channel);

    // 快捷键（前端也会处理，这里兜底，保证焦点在非输入区时也生效）
    auto* kTheme = new QShortcut(QKeySequence(QStringLiteral("Ctrl+T")), this);
    connect(kTheme, &QShortcut::activated, this, [this]() {
        m_page->runJavaScript(QStringLiteral("document.body.classList.toggle('light')"));
    });
    auto* kSearch = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this);
    connect(kSearch, &QShortcut::activated, this, [this]() {
        m_page->runJavaScript(
            QStringLiteral("(function(){var i=document.querySelector('#aiInput');"
                           "if(i){i.focus();i.select();}})()"));
    });
    auto* kFullscreen = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(kFullscreen, &QShortcut::activated, this, [this]() {
        isFullScreen() ? showNormal() : showFullScreen();
    });
    auto* kDevTools = new QShortcut(QKeySequence(Qt::Key_F12), this);
    connect(kDevTools, &QShortcut::activated, this, [this]() {
        // 专门用于排查前端问题；打包正式版时可删掉这两行
        auto* window = new QMainWindow(nullptr);
        window->setAttribute(Qt::WA_DeleteOnClose);
        auto* view = new QWebEngineView(window);
        window->setCentralWidget(view);
        window->setWindowTitle(QStringLiteral("MelodySync DevTools"));
        window->resize(1100, 700);
        m_page->setDevToolsPage(view->page());
        window->show();
    });
}

// ---------------------------------------------------------------------------
void MainWindow::loadFrontend(bool preferLocalFile)
{
    if (preferLocalFile) {
        // 开发模式：优先读磁盘上的 ui/index.html（改完刷新即可见）
        const QString local = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("../ui/index.html"));
        const QString local2 = QDir::current().filePath(QStringLiteral("ui/index.html"));
        for (const QString& cand : { local, local2 }) {
            const QString abs = QFileInfo(cand).absoluteFilePath();
            if (QFileInfo::exists(abs)) {
                qInfo() << "[Window] 开发模式，加载本地前端:" << abs;
                m_view->load(QUrl::fromLocalFile(abs));
                return;
            }
        }
        qInfo() << "[Window] 未找到本地 ui/index.html，回落到 qrc 内置版本";
    }

    // 正式模式：从 qrc 加载（resources.qrc / qt_add_resources 都会提供该路径）
    m_view->load(QUrl(QStringLiteral("qrc:/ui/index.html")));
    qInfo() << "[Window] 加载内置前端 qrc:/ui/index.html";
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_ignoreClose) {
        // 自检模式：这个会话会在窗口出现后立刻发 WM_CLOSE，窗口一关
        // WebEngine 就没了，自检跑不起来。拒绝它，等自检自己结束再退。
        qInfo() << "[Window] 收到关闭请求，但自检进行中 —— 已忽略";
        event->ignore();
        return;
    }
    qInfo() << "[Window] 收到关闭请求，退出";
    // 让前端有机会保存状态（当前实现里设置是即时落盘的，这里只做留白）
    event->accept();
}

// ---------------------------------------------------------------------------
namespace {

// 把 QString 转成可以安全嵌进 JS 源码的字符串字面量（**含两侧引号**）。
// 不能手工拼引号 —— 歌单链接或搜索词里可能出现引号、反斜杠、换行，
// 借用 JSON 的转义规则最省心：先序列化成 ["..."]，再去掉外层方括号。
QString jsLiteral(const QString& s)
{
    QJsonArray one;
    one.append(s);
    const QString arr = QString::fromUtf8(QJsonDocument(one).toJson(QJsonDocument::Compact));
    return arr.mid(1, arr.size() - 2);
}

} // namespace

void MainWindow::runSelfTest(const QString& query, const QString& playlistUrl)
{
    // 等一下再动手：qwebchannel.js 是在 DocumentCreation 阶段注入的，
    // 但前端 hydrateFromBackend() 还要走若干次「调方法 + 等信号」往返。
    const int delayMs = 8000;

    QTimer::singleShot(delayMs, this, [this, query, playlistUrl]() {
        if (!m_page) {
            qWarning() << "[Selftest] 页面未就绪，跳过";
            return;
        }

        // 第 1 步：**同步**探测页面运行环境（脚本里不含 Promise，回调必定立刻返回）。
        //
        // 为什么要有这一步：完整自检是一段 async IIFE，每个桥接调用在桥接不可用时
        // 都要等满 8 秒超时，十来个方法累计能到一分多钟，看起来就像"没反应"。
        // 先拿到这几个布尔值，就能一眼分清是「桥接根本没连上」还是「确实只是慢」。
        const QString envProbe = QStringLiteral(R"MSJS(
JSON.stringify({
  hasQWebChannel: typeof QWebChannel !== 'undefined',
  hasQt:          typeof qt !== 'undefined',
  hasTransport:   (typeof qt !== 'undefined') && !!qt.webChannelTransport,
  hasMS:          typeof MS !== 'undefined',
  hasBridgeVar:   (typeof bridge !== 'undefined') && !!bridge,
  native:         (typeof MS !== 'undefined') ? MS.native : null
})
)MSJS");
        qInfo() << "[Selftest] 步骤1 — 探测页面运行环境";
        m_page->runJavaScript(envProbe, [](const QVariant& v) {
            qInfo().noquote() << "[Selftest]   环境 =" << v.toString();
        });

        const QString script = QStringLiteral(R"MSJS(
window.__msSelftest = { done: false, result: null };
(async () => {
  const out = {};
  const fmt = v => (v === null || v === undefined) ? String(v)
              : (typeof v === 'object' ? JSON.stringify(v).slice(0, 120) : v);
  const probe = async (k, fn) => {
    try { out[k] = await fn(); }
    catch (e) { out[k] = 'JS-ERR: ' + (e && e.message ? e.message : e); }
  };

  await probe('bridge.native', () => MS.native);
  await probe('renderHelpers', () => {
    // 渲染路径的纯函数（不碰 DOM，可直接调用）。
    // 自检以前只测 MS.* 桥接，从不执行结果渲染——srcName 局部作用域事故
    // （结果页整页画不出来、用户只看到"搜了但不显示"）就是这样漏掉的。
    if (typeof srcName !== 'function') return 'FAIL srcName 未定义为全局函数';
    if (typeof srcIcon !== 'function') return 'FAIL srcIcon 未定义为全局函数';
    if (typeof _candText !== 'function') return 'FAIL _candText 未定义';
    const t = _candText([{ src: 'netease', qkey: '320000' }]);
    const n = srcName('netease');
    return (n === '网易云音乐' && String(t).indexOf('网易云音乐') >= 0)
      ? 'ok' : 'FAIL srcName=' + n + ' candText=' + t;
  });
  await probe('settings',  async () => { const s = await MS.settings();
      return s ? ('provider=' + s.aiProvider + ' model=' + s.aiModel
                  + ' baseUrl=' + s.aiBaseUrl + ' keySet=' + s.aiKeySet) : 'null'; });
  await probe('sources',   async () => { const a = await MS.sources();   return Array.isArray(a) ? a.length : fmt(a); });
  await probe('queue',     async () => { const a = await MS.queue();     return Array.isArray(a) ? a.length : fmt(a); });
  await probe('library',   async () => { const a = await MS.library();   return Array.isArray(a) ? a.length : fmt(a); });
  await probe('playlists', async () => { const a = await MS.playlists(); return Array.isArray(a) ? a.length : fmt(a); });
  await probe('history',   async () => { const a = await MS.history();   return Array.isArray(a) ? a.length : fmt(a); });
  await probe('analysis',  async () => fmt(await MS.analysis()));
  await probe('devices',   async () => { const a = await MS.devices();   return Array.isArray(a) ? a.length : fmt(a); });
  await probe('appInfo',   async () => fmt(await MS.appInfo()));

  if (__PLAYLIST__) {
    await probe('parsePlaylist', async () => {
      const r = await MS.parsePlaylist(__PLAYLIST__, 'netease');
      return !r ? 'null'
                : (r.ok ? ('OK ' + ((r.songs || []).length) + ' 首 / ' + r.name)
                        : ('FAIL ' + r.error));
    });
  }
  if (__QUERY__) {
    await probe('aiSearch', async () => {
      const r = await MS.aiSearch(__QUERY__, null);
      return !r ? 'null'
                : (r.ok ? ('OK ' + ((r.results || []).length) + ' 条 / engine=' + r.engine)
                        : ('FAIL ' + r.error));
    });
  }
  window.__msSelftest = { done: true, result: JSON.stringify(out) };
})().catch(e => {
  // 任何未捕获异常也要落结果，否则 C++ 轮询永远等不到 done —— selftest 假死。
  window.__msSelftest = { done: true,
    result: JSON.stringify({ fatal: 'JS-ERR: ' + (e && e.message ? e.message : e) }) };
});
)MSJS")
            .replace(QStringLiteral("__QUERY__"),    jsLiteral(query))
            .replace(QStringLiteral("__PLAYLIST__"), jsLiteral(playlistUrl));

        qInfo() << "[Selftest] 步骤2 — 完整桥接自检（页面加载后" << delayMs << "ms 起算）";
        m_page->runJavaScript(script);

        // ⚠ 不能指望 runJavaScript 的回调拿 async IIFE 的返回值：
        // Qt 6.5 的 runJavaScript 不等 Promise，回调当场就带着空串回来
        // （实测 9ms），脚本却还在后台继续跑 —— 结束标记因此永远不出现，
        // selftest.py 只能按假死处理。改成：脚本把结果落到全局变量，
        // C++ 每秒轮询一次，拿到 done 再输出实测表和结束标记。
        auto pollTimer = new QTimer(this);
        auto elapsedMs = std::make_shared<int>(0);
        connect(pollTimer, &QTimer::timeout, this, [this, pollTimer, elapsedMs]() {
            constexpr int kInterval = 1000;
            constexpr int kBudgetMs = 150000;   // aiSearch/parsePlaylist 全链路的最坏耗时富余
            *elapsedMs += kInterval;
            if (*elapsedMs > kBudgetMs) {
                qWarning().noquote() << "[Selftest] 轮询超时（" << kBudgetMs / 1000
                                     << "s）仍未拿到页面结果 —— 按失败处理";
                pollTimer->stop();
                pollTimer->deleteLater();
                return;
            }
            m_page->runJavaScript(
                QStringLiteral("JSON.stringify(window.__msSelftest || null)"),
                [this, pollTimer](const QVariant& v) {
                    const QJsonDocument st = QJsonDocument::fromJson(v.toString().toUtf8());
                    if (!st.isObject() || !st.object().value(QStringLiteral("done")).toBool())
                        return;                             // 还没跑完，下一轮再看
                    pollTimer->stop();
                    pollTimer->deleteLater();

                    const QString raw = st.object().value(QStringLiteral("result")).toString();
                    const QJsonObject o = QJsonDocument::fromJson(raw.toUtf8()).object();
                    if (o.isEmpty()) {
                        qWarning().noquote() << "[Selftest] 页面结果不可解析:" << raw;
                        return;
                    }
                    qInfo().noquote() << "[Selftest] ========== 桥接实测 ==========";
                    for (auto it = o.begin(); it != o.end(); ++it) {
                        qInfo().noquote() << QStringLiteral("[Selftest]   %1 = %2")
                                                 .arg(it.key().leftJustified(15),
                                                      it.value().toVariant().toString());
                    }
                    qInfo().noquote() << "[Selftest] ========== 结束 ==========";
                });
        });
        pollTimer->start(1000);
    });
}

} // namespace ms
