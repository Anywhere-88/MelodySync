// ============================================================================
//  src/core/HttpClient.h
//  基于 libcurl 的同步 HTTP 客户端。
//  约定：所有调用都在工作线程里执行（配合 core/Async.h 的 runAsync），
//        绝不从 UI 线程直接调用网络。
// ============================================================================
#pragma once

#include "core/Async.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>

namespace ms {

class HttpClient
{
public:
    struct Response
    {
        bool       ok = false;      // 传输成功且 HTTP 状态 2xx/3xx
        long       status = 0;      // HTTP 状态码，0 表示连接层失败
        QByteArray body;
        QString    error;           // 失败原因（curl_easy_strerror 或 HTTP 状态）
        QStringList headers;        // 原始响应头（用于抓 Set-Cookie，做扫码登录会话）

        // 从响应头里摘出所有 Set-Cookie 的 name=value（不含属性），便于拼 Cookie 请求头
        QString cookieHeader() const;

        QJsonObject json() const;   // 解析失败返回空对象
        QJsonArray  array() const;
        QString     text() const;
    };

    // 进程启动时调用一次（main.cpp），结束时可不管
    static void init();
    static void shutdown();

    // 通用请求
    static Response request(const QString& method,
                            const QString& url,
                            const QByteArray& body = QByteArray(),
                            const QStringList& headers = QStringList(),
                            int timeoutMs = 15000);

    static Response get(const QString& url,
                        const QStringList& headers = QStringList(),
                        int timeoutMs = 15000);

    static Response getJson(const QString& url,
                            const QStringList& headers = QStringList(),
                            int timeoutMs = 15000);

    static Response postJson(const QString& url,
                             const QJsonObject& payload,
                             const QStringList& headers = QStringList(),
                             int timeoutMs = 20000);

    static Response postRaw(const QString& url,
                            const QByteArray& body,
                            const QString& contentType,
                            const QStringList& headers = QStringList(),
                            int timeoutMs = 20000);

    static Response postForm(const QString& url,
                             const QList<QPair<QString, QString>>& fields,
                             const QStringList& headers = QStringList(),
                             int timeoutMs = 20000);

    // ---------------------------------------------------------------------
    // 文件下载（直链）。支持断点续传、进度回调与取消。
    //   onProgress 返回 false 表示中止下载
    // ---------------------------------------------------------------------
    struct DownloadResult
    {
        bool    ok = false;
        long    status = 0;
        qint64  bytes = 0;
        QString error;
        bool    canceled = false;
    };

    static DownloadResult download(const QString& url,
                                   const QString& savePath,
                                   const std::function<bool(qint64 done, qint64 total)>& onProgress = {},
                                   const CancelTokenPtr& cancel = CancelTokenPtr(),
                                   const QStringList& headers = QStringList(),
                                   int timeoutMs = 0,       // 0 = 不限制总时长
                                   bool allowResume = true);

    // ---------------------------------------------------------------------
    // 小工具
    // ---------------------------------------------------------------------
    static QString urlEncode(const QString& raw);
    static QString buildQuery(const QList<QPair<QString, QString>>& params);
    static QString userAgent();

private:
    // CA 证书：优先用 exe 同级 cacert.pem，其次回落到系统证书库
    static void applyTlsOptions(void* curlHandle);
};

} // namespace ms
