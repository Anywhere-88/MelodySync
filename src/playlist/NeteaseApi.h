// ============================================================================
//  src/playlist/NeteaseApi.h
//  网易云音乐歌单解析 + 扫码登录。
//
//  合规说明：
//    - 只读取歌单的**元信息**（歌名/歌手/专辑/时长/可用音质），
//      用于让用户确认"要下载哪几首"，不获取任何音频地址；
//    - 扫码登录使用网易云官方的 qrcode/unikey + client/login 流程，
//      等价于在官方网页上登录，不做任何签名伪造；
//    - 私有歌单必须用户本人扫码授权后才能读取，工具不缓存账号密码。
// ============================================================================
#pragma once

#include "core/Types.h"

#include <QString>

namespace ms {

class NeteaseApi
{
public:
    struct QrSession
    {
        QString unikey;
        QString qrContent;      // 需要编码成二维码的内容（前端自己渲染）
        QString qrImageUrl;
    };

    NeteaseApi() = default;

    static bool    isNeteaseUrl(const QString& url);
    static QString extractPlaylistId(const QString& url);

    // 解析歌单：公开歌单无需登录；私有歌单需先用 pollQr 拿到 cookie
    bool parsePlaylist(const QString& url, ParsedPlaylist* out, QString* errorOut);

    // ---- 扫码登录 ----
    QrSession createQrSession(QString* errorOut);
    // 返回码：800 二维码过期 / 801 等待扫码 / 802 待确认 / 803 授权成功
    int  pollQr(const QString& unikey, QString* nickname, QString* errorOut);

    void    setCookie(const QString& cookie) { m_cookie = cookie; }
    QString cookie() const { return m_cookie; }
    bool    loggedIn() const { return !m_cookie.isEmpty(); }

    // 从曲目 JSON 里推断可用音质（网易云的 h/m/l/sq/hr 字段结构）
    static Quality inferQuality(const QJsonObject& track);

private:
    QJsonObject fetchPlaylistJson(const QString& playlistId, QString* errorOut);

    QString m_cookie;
    QString m_userAgent = QStringLiteral(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0 Safari/537.36");
};

} // namespace ms
