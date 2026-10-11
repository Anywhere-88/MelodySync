// ============================================================================
//  src/bridge/LocalAudioServer.h
//  本地音频 HTTP 服务：给前端 <audio> 提供能真正播出来的 URL。
//
//  为什么必须有它：前端页面是 qrc:/ui/index.html，Chromium 把 file:// 当
//  跨源子资源一律拦截 —— 直接喂 file:///C:/... 给 <audio> 必然
//  NotAllowedError/NETWORK_ERR（实测：文件明明在，toast 却报"文件可能已被
//  移动或删除"）。而 MainWindow 里早已打开的 LocalContentCanAccessFileUrls
//  管不了这种情况（qrc 页面不被视为 file 源）。
//
//  解法：在 127.0.0.1 上起一个只读的小 HTTP 服务，
//    GET /a/<base64url(绝对路径)>
//  返回音频字节流。支持 HTTP Range（<audio> 拖进度条靠它，Chromium 首个
//  请求也常带 bytes=0-）。只允许音频扩展名 + 文件必须存在。
// ============================================================================
#pragma once

#include <QTcpServer>
#include <QString>

class QTcpSocket;

namespace ms {

class LocalAudioServer : public QTcpServer
{
    Q_OBJECT

public:
    static LocalAudioServer& instance();

    // 绑定 127.0.0.1 随机空闲端口。重复调用是安全的（已启动就直接返回）。
    bool start();

    int port() const { return m_port; }

    // 前端拿到的播放地址；路径不合法 / 服务未启动时返回空串。
    QString audioUrl(const QString& absolutePath) const;

    // 音频扩展名白名单（<audio> 能解码的 + 本应用下载产物的全部格式）
    static bool isAudioFile(const QString& path);

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private:
    LocalAudioServer() = default;
    int m_port = 0;
};

} // namespace ms
