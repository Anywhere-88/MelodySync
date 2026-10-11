// ============================================================================
//  src/search/sources/PixabaySource.h
//  Pixabay Music —— ⚠ 本源当前**未注册**（见 SearchAggregator::registerDefaults）
//
//  什么原因停用（已核实，不是猜测）：
//    Pixabay 官方 API 只有两个端点：
//        https://pixabay.com/api/          —— 图片 / 插画 / 矢量
//        https://pixabay.com/api/videos/   —— 视频
//    **没有任何公开的音乐或音效检索端点**。原先这里请求的
//        https://pixabay.com/api/music/
//    必然 404，日志里就是那一行：
//        [Pixabay] 检索不可用（源将跳过）: "HTTP 404"
//    换句话说，这个源从第一天起就没返回过任何一条结果。
//
//    曾经注释里写的"使用站点的公开 JSON 端点（无需登录）"是错的：那个端点
//    不存在。图片/视频的 API Key 与音频不通用，让用户去申请 key 也救不回来，
//    所以直接停用，而不是把一个永远空的源留在设置页里让人怀疑是自己配置错了。
//
//  保留文件的意义：接口和 meta() 还在，将来官方若开放音频端点可以直接接回来。
// ============================================================================
#pragma once

#include "search/SearchSource.h"

namespace ms {

class PixabaySource : public SearchSource
{
public:
    explicit PixabaySource(const QString& apiKey = QString());

    SourceMeta meta() const override;
    QVector<SearchHit> search(const SearchContext& ctx) override;

private:
    QString m_apiKey;
};

} // namespace ms
