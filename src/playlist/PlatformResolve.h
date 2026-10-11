// ============================================================================
//  src/playlist/PlatformResolve.h
//  平台音频直链解析（网易云 / QQ 音乐）
//
//  背景（为什么需要这个模块）：
//    搜索阶段各平台返回的只是曲目元信息，真正能播/能存的地址要另外换一次。
//    以前这一步被"合规"顾虑整个砍掉了 —— 结果就是搜索出来的每条都只能试听，
//    用户看到「仅试听」按钮却没有任何可下载的东西。
//
//  现在按平台官方网页版播放器**自己用的公开接口**取地址：
//    - 网易云：GET  /api/song/enhance/player/url?ids=[...]&br=...
//              返回真实 CDN 直链 + 码率 + 字节数 + 类型，未登录也能拿到非会员曲目；
//              有扫码登录 cookie 时会拿到更高码率。
//    - QQ  ：POST https://u.y.qq.com/cgi-bin/musicu.fcg
//              module=vkey.GetVkeyServer 换 purl，按 无损→320→128 顺序探测，
//              能拿到哪档用哪档；需要会员的档位平台会返回非 0 的 result 码，
//              我们据此如实标记"该曲目需付费"，而不是伪造。
//
//  两条硬约束（踩过坑，不要改）：
//    1) 返回的直链是**带时效**的（网易云 expi=1200 秒，QQ 的 vkey 同理会过期）。
//       所以绝对不能把地址长期存进队列 —— 下载前必须重新解析一次。
//    2) 直链必须带 Referer 才能拉：网易云要 music.163.com，QQ 要 y.qq.com。
//       漏了会被 CDN 返回 403，且错误信息看不出原因。
// ============================================================================
#pragma once

#include "core/Types.h"

#include <QHash>
#include <QString>
#include <QStringList>

namespace ms {

// 一条已解析出来的音频地址
struct DirectAudio
{
    QString url;                 // 完整直链（含鉴权参数），空 = 拿不到
    int     br        = 0;       // 比特率 bps，0 = 未知
    qint64  sizeBytes = 0;       // 平台声明的字节数，0 = 未知
    QString type;                // mp3 / flac / m4a / aac
    bool    ok        = false;   // url 非空且平台确认可用
    bool    needVip   = false;   // 平台明确回复"要会员/要付费"
    QString error;               // 拿不到时可读的原因

    QString extension() const;   // 由 type / url 推断落盘扩展名
    Quality quality() const;     // 由 br + type 推断音质
};

class PlatformResolve
{
public:
    // ---- 网易云 ----
    // 单曲；cookie 可为空。preferBr 用 999000 表示"给我可用的最高档"。
    static DirectAudio netease(const QString& songId,
                               const QString& cookie = QString(),
                               QString* errorOut = nullptr);
    // 批量：一次请求搞定整页搜索结果，避免每条一次往返
    static QHash<QString, DirectAudio> neteaseBatch(const QStringList& songIds,
                                                    const QString& cookie = QString(),
                                                    QString* errorOut = nullptr);

    // ---- QQ 音乐 ----
    static DirectAudio qq(const QString& songMid,
                          const QString& cookie = QString(),
                          QString* errorOut = nullptr);
    static QHash<QString, DirectAudio> qqBatch(const QStringList& songMids,
                                               const QString& cookie = QString(),
                                               QString* errorOut = nullptr);

    // ---- 通用 ----
    // 从 "netease:12345" 里取出平台内 id。没有冒号时原样返回（容错）。
    static QString platformIdOf(const QString& globalId);

    // 该源是否支持直链解析（走 yt-dlp 的源返回 false）
    static bool supports(const QString& sourceId);

    // 下载直链时必须带的请求头
    static QStringList headersFor(const QString& sourceId);
};

} // namespace ms
