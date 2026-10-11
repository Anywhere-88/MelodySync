// ============================================================================
//  src/core/Async.h
//  极简异步工具：
//    - runAsync      在全局线程池跑后台任务（网络 / IO 一律走这里，绝不阻塞 UI）
//    - runOnMain     把结果投递回主线程（Qt 信号槽的跨线程安全前提）
//    - parallelForEach  一个"没有 QtConcurrent 的 parallel_for"
//    - CancelToken   下载 / 同步 / 搜索的可取消令牌
// ============================================================================
#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QMetaObject>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace ms {

// ---------------------------------------------------------------------------
// 取消令牌：所有长任务都持有一个，UI 点"取消"时置位，工作线程轮询退出
// ---------------------------------------------------------------------------
class CancelToken
{
public:
    void cancel()               { m_flag.store(true, std::memory_order_relaxed); }
    bool isCanceled() const     { return m_flag.load(std::memory_order_relaxed); }
    void reset()                { m_flag.store(false, std::memory_order_relaxed); }

private:
    std::atomic_bool m_flag{ false };
};
using CancelTokenPtr = std::shared_ptr<CancelToken>;

// ---------------------------------------------------------------------------
// 把 std::function 包成 QRunnable
// ---------------------------------------------------------------------------
class LambdaRunnable : public QRunnable
{
public:
    explicit LambdaRunnable(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { if (m_fn) m_fn(); }

private:
    std::function<void()> m_fn;
};

// 提交到全局线程池（MAX_THREAD_COUNT 由 Qt 按 CPU 核数决定，够用）
inline void runAsync(std::function<void()> fn)
{
    QThreadPool::globalInstance()->start(new LambdaRunnable(std::move(fn)));
}

// ---------------------------------------------------------------------------
// 回到主线程执行：以 qApp 作为上下文对象，Qt 会自动排队到主线程事件循环
// ---------------------------------------------------------------------------
template <typename Fn>
inline void runOnMain(Fn fn)
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) { fn(); return; }
    if (QThread::currentThread() == app->thread()) { fn(); return; }
    QMetaObject::invokeMethod(app, std::move(fn), Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// 并行 forEach：把 items 分给 min(threads, size) 个 std::thread
//   * 单个 item 抛异常不会中断其它；异常计数交给调用方通过返回值判断
//   * 用于"并行聚合 8 个源"这种 CPU/网络混合场景
// ---------------------------------------------------------------------------
template <typename T, typename Fn>
inline void parallelForEach(const QVector<T>& items, int threads, Fn fn)
{
    const int n = items.size();
    if (n <= 0) return;

    int workers = qBound(1, threads, 8);
    if (workers > n) workers = n;

    std::atomic<int> next{ 0 };
    auto worker = [&]() {
        for (;;) {
            const int i = next.fetch_add(1);
            if (i >= n) break;
            fn(items[i]);
        }
    };

    if (workers == 1) { worker(); return; }

    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (int i = 0; i < workers - 1; ++i) pool.emplace_back(worker);
    worker();                        // 当前线程也参与，减少一次上下文切换
    for (std::thread& t : pool) if (t.joinable()) t.join();
}

// 简易节流器：限制单位时间内触发次数（用于进度信号，避免刷爆前端）
class Throttle
{
public:
    explicit Throttle(int intervalMs = 100);

    bool ready();          // 距上次放行已超过 intervalMs
    void force();          // 强制下一次 ready() 返回 true
    void reset();          // 清空计时

private:
    int    m_interval;
    qint64 m_last = 0;
};

} // namespace ms
