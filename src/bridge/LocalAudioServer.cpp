// ============================================================================
//  src/bridge/LocalAudioServer.cpp
// ============================================================================
#include "bridge/LocalAudioServer.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace ms {

namespace {

// 每个连接一个会话对象：持有 socket + 打开的文件，按 chunk 异步发送。
// 为什么分块而不是一把 write：整文件 write 会把几十 MB 堆进 Qt 的发送缓冲
// （客户端消费慢时内存直接涨到文件大小）。bytesWritten 驱动的分块发送
// 内存占用恒定，也不会阻塞主线程 —— 这是 Qt 的标准做法。
//
// 注意：这个类故意不写 Q_OBJECT —— 它自己不发信号，所有回调都用 lambda
// 挂在 socket 的信号上（lambda 的 context object 只要求基类是 QObject）。
// 在 .cpp 的匿名命名空间里放 Q_OBJECT 类会让 AUTOMOC 多生成一份需要手工
// include 的 moc 输出，纯粹是自找麻烦。
class AudioSession : public QObject
{
public:
    explicit AudioSession(qintptr socketDescriptor)
    {
        m_sock = new QTcpSocket(this);
        m_sock->setSocketDescriptor(socketDescriptor);
        connect(m_sock, &QTcpSocket::disconnected, this, &QObject::deleteLater);
        connect(m_sock, &QTcpSocket::bytesWritten, this,
                [this](qint64) { pump(); });
        // 兜底：头 15 秒还没到齐的连接直接掐掉，不让死连接占着 fd。
        // 只在"还没开始发正文"时生效 —— 正常传输一旦开始，m_started 置位。
        QTimer::singleShot(15000, this, [this]() {
            if (!m_started) m_sock->abort();
        });
    }

    void handle()
    {
        // 请求头没到齐：等下一个 readyRead 再进来一次
        if (!m_sock->canReadLine()) {
            connect(m_sock, &QTcpSocket::readyRead, this, [this]() {
                disconnect(m_sock, &QTcpSocket::readyRead, this, nullptr);
                handle();
            });
            return;
        }

        const QString requestLine = QString::fromUtf8(m_sock->readLine()).trimmed();
        // 只吃 GET /a/<token>。token = base64url(绝对路径, UTF-8)
        static const QRegularExpression re(QStringLiteral("^GET /a/([^\\s?]+)"));
        const QRegularExpressionMatch m = re.match(requestLine);
        if (!m.hasMatch()) { reject(400, "bad request"); return; }

        const QByteArray token =
            QUrl::fromPercentEncoding(m.captured(1).toUtf8()).toUtf8();
        const QByteArray raw = QByteArray::fromBase64(
            token, QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        const QString path = QString::fromUtf8(raw);

        // 双重门槛：音频扩展名 + 文件真实存在。这个服务绑定在 127.0.0.1，
        // 但同一台机器上的其他进程也能访问 —— 不能变成"任意文件读取"。
        if (!LocalAudioServer::isAudioFile(path) || !QFileInfo::exists(path)) {
            reject(404, "not found");
            return;
        }

        // Range 解析：Chromium 会发 "bytes=0-"、"bytes=12345-"，也可能带结尾。
        // 不带 Range 的请求一概回 200 全量（响应头里声明了 Accept-Ranges）。
        qint64 start = 0, end = -1;
        bool hasRange = false;
        while (m_sock->canReadLine()) {
            const QString line = QString::fromUtf8(m_sock->readLine()).trimmed();
            if (line.isEmpty()) break;
            if (line.startsWith(QLatin1String("Range:"), Qt::CaseInsensitive)) {
                static const QRegularExpression rangeRe(
                    QStringLiteral("bytes=(\\d*)-(\\d*)"));
                const QRegularExpressionMatch rm = rangeRe.match(line);
                if (rm.hasMatch()) {
                    hasRange = true;
                    start = rm.captured(1).toLongLong();
                    if (!rm.captured(2).isEmpty()) end = rm.captured(2).toLongLong();
                }
            }
        }

        m_file.setFileName(path);
        if (!m_file.open(QIODevice::ReadOnly)) { reject(403, "cannot open"); return; }

        const qint64 total = m_file.size();
        if (end < 0 || end >= total) end = total - 1;
        m_remaining = end - start + 1;
        if (start >= total || m_remaining <= 0) { reject(416, "range error"); return; }
        m_file.seek(start);

        QByteArray head;
        head += hasRange ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
        head += "Content-Type: " + contentType(path).toUtf8() + "\r\n";
        head += "Accept-Ranges: bytes\r\n";
        // CORS：悦动音符页的镜像 <audio>（crossOrigin=anonymous）要对同一 URL
        // 再拉一遍做频谱分析；qrc 页面是 opaque origin，必须用通配符放行。
        // 只绑 127.0.0.1 + 音频扩展名双重门槛，放开 CORS 不扩大文件暴露面。
        head += "Access-Control-Allow-Origin: *\r\n";
        head += "Cache-Control: no-store\r\n";
        if (hasRange)
            head += "Content-Range: bytes " + QByteArray::number(start) + "-"
                    + QByteArray::number(end) + "/" + QByteArray::number(total)
                    + "\r\n";
        head += "Content-Length: " + QByteArray::number(m_remaining) + "\r\n";
        head += "Connection: close\r\n\r\n";
        m_sock->write(head);
        m_started = true;
        pump();      // 头部进了缓冲就会触发 bytesWritten，这里先主动推一把
    }

private:
    void pump()
    {
        if (!m_sock || m_sock->state() != QAbstractSocket::ConnectedState) return;

        // 背压控制：未确认的发送缓冲超过 1MB 就先不读盘，等 bytesWritten 再续
        while (m_remaining > 0 && m_sock->bytesToWrite() < 1024 * 1024) {
            const QByteArray chunk = m_file.read(
                static_cast<qint64>(qMin<qint64>(256 * 1024, m_remaining)));
            if (chunk.isEmpty()) break;      // 文件比声明的小（磁盘上被动过）
            m_sock->write(chunk);
            m_remaining -= chunk.size();
        }

        if (m_remaining <= 0 && m_sock->bytesToWrite() == 0)
            m_sock->disconnectFromHost();    // 发完即断，<audio> 才会触发 ended
    }

    void reject(int code, const char* text)
    {
        const QByteArray head = "HTTP/1.1 " + QByteArray::number(code) + " "
                                + text + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        m_sock->write(head);
        m_sock->disconnectFromHost();
    }

    static QString contentType(const QString& path)
    {
        const QString e = QFileInfo(path).suffix().toLower();
        if (e == QLatin1String("mp3"))  return QStringLiteral("audio/mpeg");
        if (e == QLatin1String("flac")) return QStringLiteral("audio/flac");
        if (e == QLatin1String("m4a") || e == QLatin1String("aac"))
            return QStringLiteral("audio/mp4");
        if (e == QLatin1String("wav"))  return QStringLiteral("audio/wav");
        if (e == QLatin1String("ogg") || e == QLatin1String("opus"))
            return QStringLiteral("audio/ogg");
        if (e == QLatin1String("ape"))  return QStringLiteral("audio/x-ape");
        return QStringLiteral("application/octet-stream");
    }

    QTcpSocket* m_sock = nullptr;
    QFile m_file;
    qint64 m_remaining = 0;
    bool m_started = false;
};

} // namespace

LocalAudioServer& LocalAudioServer::instance()
{
    static LocalAudioServer s_server;
    return s_server;
}

bool LocalAudioServer::isAudioFile(const QString& path)
{
    static const QSet<QString> kAudioExt = {
        QStringLiteral("mp3"),  QStringLiteral("flac"), QStringLiteral("m4a"),
        QStringLiteral("aac"),  QStringLiteral("wav"),  QStringLiteral("ogg"),
        QStringLiteral("opus"), QStringLiteral("ape"),
    };
    const QString e = QFileInfo(path).suffix().toLower();
    return !e.isEmpty() && kAudioExt.contains(e);
}

bool LocalAudioServer::start()
{
    if (m_port != 0) return true;          // 已在监听
    // 只绑回环地址：外部机器连不上，不向局域网暴露任何文件
    if (!listen(QHostAddress(QHostAddress::LocalHost))) {
        qWarning() << "[AudioSrv] 监听失败:" << errorString();
        return false;
    }
    m_port = serverPort();
    qInfo() << "[AudioSrv] 本地音频服务已就绪: 127.0.0.1:" << m_port;
    return true;
}

QString LocalAudioServer::audioUrl(const QString& absolutePath) const
{
    if (m_port == 0) return QString();
    const QString clean = QDir::cleanPath(absolutePath);
    if (!isAudioFile(clean) || !QFileInfo::exists(clean)) return QString();
    const QByteArray token = QByteArray(clean.toUtf8())
                                 .toBase64(QByteArray::Base64UrlEncoding
                                           | QByteArray::OmitTrailingEquals);
    return QStringLiteral("http://127.0.0.1:%1/a/%2").arg(m_port).arg(
        QString::fromLatin1(token));
}

void LocalAudioServer::incomingConnection(qintptr socketDescriptor)
{
    // 会话自管理：断开即 deleteLater，服务本身不持有任何长期状态
    auto* session = new AudioSession(socketDescriptor);
    session->handle();
}

} // namespace ms
