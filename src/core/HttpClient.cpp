// ============================================================================
//  src/core/HttpClient.cpp
// ============================================================================
#include "core/HttpClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>

#include <curl/curl.h>

#include <mutex>

namespace ms {

namespace {

std::once_flag g_initFlag;

// libcurl 写回调 -> QByteArray
size_t writeToBuffer(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* buf = static_cast<QByteArray*>(userdata);
    const size_t n = size * nmemb;
    buf->append(ptr, static_cast<int>(n));
    return n;
}

// 响应头回调：逐行收集，用于抓 Set-Cookie
size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
    auto* list = static_cast<QStringList*>(userdata);
    const size_t n = size * nitems;
    const QString line = QString::fromUtf8(buffer, static_cast<int>(n)).trimmed();
    if (!line.isEmpty()) list->append(line);
    return n;
}

// 文件下载上下文
struct DownloadCtx
{
    QFile* file = nullptr;
    const std::function<bool(qint64, qint64)>* onProgress = nullptr;
    CancelTokenPtr cancel;
    bool aborted = false;
};

size_t writeToFile(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* ctx = static_cast<DownloadCtx*>(userdata);
    const qint64 n = static_cast<qint64>(size * nmemb);
    if (ctx->file && ctx->file->write(ptr, n) != n) return 0;   // 返回 0 => curl 报 CURLE_WRITE_ERROR
    return static_cast<size_t>(n);
}

int progressCallback(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
{
    auto* ctx = static_cast<DownloadCtx*>(userdata);
    if (ctx->cancel && ctx->cancel->isCanceled()) { ctx->aborted = true; return 1; }  // 非 0 = 中止
    if (ctx->onProgress && *ctx->onProgress) {
        if (!(*ctx->onProgress)(static_cast<qint64>(dlnow), static_cast<qint64>(dltotal))) {
            ctx->aborted = true;
            return 1;
        }
    }
    return 0;
}

struct curl_slist* makeHeaders(const QStringList& headers, struct curl_slist*& head)
{
    for (const QString& h : headers) {
        const QByteArray line = h.toUtf8();
        head = curl_slist_append(head, line.constData());
    }
    return head;
}

} // namespace

// ---------------------------------------------------------------------------
void HttpClient::init()
{
    std::call_once(g_initFlag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

void HttpClient::shutdown()
{
    // 进程退出时不必强求，交给 OS 回收；这里保留接口便于单元测试
}

QJsonObject HttpClient::Response::json() const
{
    if (body.isEmpty()) return QJsonObject();
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    return doc.isObject() ? doc.object() : QJsonObject();
}

QJsonArray HttpClient::Response::array() const
{
    if (body.isEmpty()) return QJsonArray();
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    return doc.isArray() ? doc.array() : QJsonArray();
}

QString HttpClient::Response::text() const { return QString::fromUtf8(body); }

// 把多个 Set-Cookie 折叠成一个 Cookie 请求头（只保留 name=value，丢掉 Path/Expires 等属性）
QString HttpClient::Response::cookieHeader() const
{
    QStringList pairs;
    for (const QString& h : headers) {
        if (!h.startsWith(QStringLiteral("set-cookie:"), Qt::CaseInsensitive)) continue;
        const QString kv = h.mid(h.indexOf(QLatin1Char(':')) + 1)
                              .split(QLatin1Char(';')).value(0).trimmed();
        if (!kv.isEmpty()) pairs << kv;
    }
    return pairs.join(QStringLiteral("; "));
}

// ---------------------------------------------------------------------------
void HttpClient::applyTlsOptions(void* handle)
{
    CURL* curl = static_cast<CURL*>(handle);

    // 优先使用随程序分发的 cacert.pem（企业内网/自签代理环境下更稳）
    const QString ca = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("cacert.pem"));
    if (QFileInfo::exists(ca)) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca.toUtf8().constData());
    } else {
        // 回落到 Windows 系统证书库（curl 内置 schannel/openssl 会自动选择）
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
}

QString HttpClient::userAgent()
{
    return QStringLiteral("MelodySync/1.0 (Windows; Qt; +https://example.local)");
}

QString HttpClient::urlEncode(const QString& raw)
{
    QByteArray in = raw.toUtf8();
    char* enc = curl_easy_escape(nullptr, in.constData(), in.size());
    if (!enc) return raw;
    const QString out = QString::fromUtf8(enc);
    curl_free(enc);
    return out;
}

QString HttpClient::buildQuery(const QList<QPair<QString, QString>>& params)
{
    QStringList parts;
    parts.reserve(params.size());
    for (const auto& p : params)
        parts << (urlEncode(p.first) + QLatin1Char('=') + urlEncode(p.second));
    return parts.join(QLatin1Char('&'));
}

// ---------------------------------------------------------------------------
HttpClient::Response HttpClient::request(const QString& method,
                                         const QString& url,
                                         const QByteArray& body,
                                         const QStringList& headers,
                                         int timeoutMs)
{
    init();

    Response res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        res.error = QStringLiteral("curl_easy_init 失败");
        return res;
    }

    QByteArray responseBuf;
    QByteArray bodyCopy = body;
    struct curl_slist* head = nullptr;
    makeHeaders(headers, head);
    {
        const QByteArray ua = (QStringLiteral("User-Agent: ") + userAgent()).toUtf8();
        head = curl_slist_append(head, ua.constData());
    }

    const QByteArray urlUtf8 = url.toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, head);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res.headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    if (timeoutMs > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    applyTlsOptions(curl);

    const QByteArray m = method.toUpper().toUtf8();
    if (m == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(bodyCopy.size()));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyCopy.isEmpty() ? "" : bodyCopy.constData());
    } else if (m == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(bodyCopy.size()));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyCopy.isEmpty() ? "" : bodyCopy.constData());
    } else if (m != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, m.constData());
    }

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        res.error = QString::fromUtf8(curl_easy_strerror(rc));
        res.status = 0;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
        res.body = responseBuf;
        res.ok = (res.status >= 200 && res.status < 400);
        if (!res.ok) res.error = QStringLiteral("HTTP %1").arg(res.status);
    }

    curl_slist_free_all(head);
    curl_easy_cleanup(curl);
    return res;
}

HttpClient::Response HttpClient::get(const QString& url, const QStringList& headers, int timeoutMs)
{
    return request(QStringLiteral("GET"), url, QByteArray(), headers, timeoutMs);
}

HttpClient::Response HttpClient::getJson(const QString& url, const QStringList& headers, int timeoutMs)
{
    QStringList h = headers;
    h << QStringLiteral("Accept: application/json");
    return get(url, h, timeoutMs);
}

HttpClient::Response HttpClient::postJson(const QString& url,
                                          const QJsonObject& payload,
                                          const QStringList& headers,
                                          int timeoutMs)
{
    QStringList h = headers;
    h << QStringLiteral("Content-Type: application/json; charset=utf-8");
    h << QStringLiteral("Accept: application/json");
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    return request(QStringLiteral("POST"), url, body, h, timeoutMs);
}

HttpClient::Response HttpClient::postRaw(const QString& url,
                                         const QByteArray& body,
                                         const QString& contentType,
                                         const QStringList& headers,
                                         int timeoutMs)
{
    QStringList h = headers;
    h << (QStringLiteral("Content-Type: ") + contentType);
    return request(QStringLiteral("POST"), url, body, h, timeoutMs);
}

HttpClient::Response HttpClient::postForm(const QString& url,
                                          const QList<QPair<QString, QString>>& fields,
                                          const QStringList& headers,
                                          int timeoutMs)
{
    return postRaw(url, buildQuery(fields).toUtf8(),
                   QStringLiteral("application/x-www-form-urlencoded"),
                   headers, timeoutMs);
}

// ---------------------------------------------------------------------------
HttpClient::DownloadResult HttpClient::download(const QString& url,
                                                const QString& savePath,
                                                const std::function<bool(qint64, qint64)>& onProgress,
                                                const CancelTokenPtr& cancel,
                                                const QStringList& headers,
                                                int timeoutMs,
                                                bool allowResume)
{
    init();

    DownloadResult res;
    QDir().mkpath(QFileInfo(savePath).absolutePath());

    // 断点续传：已存在且非空的文件从已有长度继续
    qint64 already = 0;
    QFile file(savePath);
    QIODevice::OpenMode mode = QIODevice::WriteOnly;
    if (allowResume && QFileInfo::exists(savePath)) {
        already = QFileInfo(savePath).size();
        mode = QIODevice::Append;
    }
    if (!file.open(mode)) {
        res.error = QStringLiteral("无法写入文件：%1").arg(savePath);
        return res;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        res.error = QStringLiteral("curl_easy_init 失败");
        return res;
    }

    DownloadCtx ctx;
    ctx.file = &file;
    ctx.onProgress = &onProgress;
    ctx.cancel = cancel;

    struct curl_slist* head = nullptr;
    makeHeaders(headers, head);
    {
        const QByteArray ua = (QStringLiteral("User-Agent: ") + userAgent()).toUtf8();
        head = curl_slist_append(head, ua.constData());
    }

    const QByteArray urlUtf8 = url.toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, head);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);      // 1 字节/秒……
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);      // ……持续 60 秒即判定断流
    if (timeoutMs > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    if (already > 0) curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(already));
    applyTlsOptions(curl);

    const CURLcode rc = curl_easy_perform(curl);
    file.flush();
    const qint64 written = file.size();
    file.close();

    if (rc != CURLE_OK) {
        res.canceled = ctx.aborted;
        res.error = ctx.aborted ? QStringLiteral("已取消")
                                : QString::fromUtf8(curl_easy_strerror(rc));
        res.status = 0;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
        res.bytes = written;
        res.ok = (res.status >= 200 && res.status < 400) && written > 0;
        if (!res.ok) res.error = QStringLiteral("HTTP %1").arg(res.status);
    }

    curl_slist_free_all(head);
    curl_easy_cleanup(curl);
    return res;
}

} // namespace ms
