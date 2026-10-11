// ============================================================================
//  src/core/Async.cpp
//
//  只实现 Throttle。其余工具（runAsync / runOnMain / parallelForEach）都是
//  inline 模板，定义在 Async.h 里，没有独立的编译单元。
//
//  为什么需要 Throttle：
//      下载引擎每收到一块数据就想往前端推一次进度。libcurl 的写回调在
//      40MB/s 的带宽下每秒能被调用上千次，如果每次都 emit 一个 Qt 信号，
//      信号会跨线程排队进主线程事件循环，再经 QWebChannel 序列化成 JSON
//      丢给 V8 —— 前端光渲染进度条就吃掉大量 CPU，而进度条一秒刷 60 次
//      和人眼看 15 次没有任何区别。所以统一用 Throttle(100ms) 限流，
//      保证「最多每 100ms 推一次」。
// ============================================================================
#include "core/Async.h"

#include <QDateTime>

namespace ms {

// intervalMs：最小放行间隔（毫秒）。传 0 表示不限流（每次都放行）。
Throttle::Throttle(int intervalMs)
    : m_interval(intervalMs)
    , m_last(0)          // 初始为 0：第一次 ready() 一定放行（见下面说明）
{
}

// 判断现在能不能放行；能放行则顺手把时间戳推进到现在。
//
// 注意这里的写法刻意不用「定时器 + 累积计数」：
//   * 定时器方案需要事件循环参与，而 ready() 是在 libcurl 的工作线程里被
//     同步调用的，那个线程根本没有 Qt 事件循环；
//   * 用单调递增的 QDateTime::currentMSecsSinceEpoch() 做差是最便宜的判断，
//     一次整数比较，不需要锁 —— 因为每个 DownloadTask 只被一个工作线程
//     持有，Throttle 实例本身不会被并发访问。
bool Throttle::ready()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_last >= m_interval) {
        m_last = now;
        return true;
    }
    return false;
}

// 强制放行一次。
//
// 实现上是把时间戳抹成 0 而不是置一个"已过期"标志位：
// currentMSecsSinceEpoch() 是从 1970 年起的毫秒数（约 1.7e12），
// 减去 0 必然远大于任何合理的 interval，于是下一次 ready() 直接命中
// 第一个分支。少一个成员变量，也少一条分支。
//
// 典型用途：任务从"下载中"进入"已完成"的瞬间，必须立刻推一次 100%
// 的终态给前端，不能被 100ms 的节流窗口吃掉。
void Throttle::force() { m_last = 0; }

// 把计时基准拉回"现在"，即接下来 intervalMs 毫秒内都静默。
//
// 与 force() 恰好相反，用于任务重试 / 恢复的场景：
// 刚恢复时先别急着推进度，等状态稳定了再开始报，避免前端进度条
// 出现"瞬间回跳到旧值"的抖动。
void Throttle::reset() { m_last = QDateTime::currentMSecsSinceEpoch(); }

} // namespace ms
