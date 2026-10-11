// ============================================================================
//  src/MainWindow.h
//  主窗口：QMainWindow + QWebEngineView（前端界面）+ QWebChannel（桥接）。
//
//  为什么用 QWebEngine 而不是 QWidget 原生控件：
//    - 界面层用 HTML/CSS 迭代速度远超 Qt Widgets，动效与玻璃质感也好做得多；
//    - 前端可以脱离 Qt 在浏览器里独立开发调试（index.html 自带 mock 后端）；
//    - C++ 只负责"重活"（网络、转码、设备、数据库），职责边界清晰。
//
//  qwebchannel.js 的加载方式（容易踩坑）：
//    不要指望在 HTML 里写 <script src="qrc:///qtwebchannel/qwebchannel.js">
//    就能生效 —— 用 qrc: 协议加载页面时，浏览器安全策略可能拦截该请求。
//    正确做法是用 QWebEngineScript 在 DocumentCreation 阶段注入（见 cpp），
//    保证在任何 URL scheme 下 QWebChannel 都已就绪。
// ============================================================================
#pragma once

#include <QMainWindow>

QT_BEGIN_NAMESPACE
class QWebEngineView;
class QWebEngineProfile;
class QWebEnginePage;
QT_END_NAMESPACE

namespace ms {

class WebBridge;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    WebBridge* bridge() const { return m_bridge; }

    // 加载前端：默认从 qrc 里读（打成单 exe）；置 true 时读本地 ui/index.html，
    // 方便开发时改前端代码后直接刷新，不用重新编译。
    void loadFrontend(bool preferLocalFile = false);

    // 桥接自检（由 main.cpp 在 --selftest 时调用）。
    //
    // 为什么需要它：前端跑在 QWebEngine 里，桥接契约是「调方法 + 等信号」。
    // 这类问题（信号名对不上、返回值忘了 await、超时回落成 mock）静态检查
    // 和单元测试都抓不到，只有真正在页面里执行一次才暴露。
    // 这个方法会在页面就绪后往 JS 里注入一段自检脚本，把每个桥接方法的
    // 真实返回类型打到日志里 —— 相当于把"人工点一遍界面"自动化。
    void runSelfTest(const QString& query, const QString& playlistUrl);

    // 让窗口拒绝外部的关闭请求（自检模式下用）。
    //
    // 某些无人值守 / 受限会话会在窗口出现后立刻发 WM_CLOSE，窗口一关
    // WebEngine 也跟着销毁，自检根本来不及跑；而窗口不显示又不行 ——
    // Chromium 会把不可见页面的 JS 挂起，runJavaScript 的回调永远不触发。
    // 两条路都堵死，只能"显示窗口但拒绝被关掉"。
    void setIgnoreClose(bool on) { m_ignoreClose = on; }

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void setupWebEngine();
    void setupBridge();
    void applyDarkPalette();

    QWebEngineView* m_view = nullptr;
    QWebEnginePage* m_page = nullptr;
    WebBridge*      m_bridge = nullptr;
    bool            m_ignoreClose = false;
};

} // namespace ms
