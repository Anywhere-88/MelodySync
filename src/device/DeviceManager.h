// ============================================================================
//  src/device/DeviceManager.h
//  设备发现 + USB 热插拔监听。
//
//  热插拔原理（Windows）：
//    窗口收到 WM_DEVICECHANGE 消息 → 我们通过 QAbstractNativeEventFilter
//    在 Qt 的事件循环之前截获它。DBT_DEVICEARRIVAL = 插入，
//    DBT_DEVICEREMOVECOMPLETE = 拔出（包括"未安全弹出"的暴力拔出）。
//    注意：一次插拔系统会连发好几条消息（不同 DBT_DEVTYP 各一条），
//    所以刷新动作必须去抖（debounce），否则会在瞬间做 N 次全盘枚举。
// ============================================================================
#pragma once

#include "core/Types.h"

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QTimer>
#include <QVector>

namespace ms {

class DeviceManager : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit DeviceManager(QObject* parent = nullptr);
    ~DeviceManager() override;

    void start();      // 安装事件过滤器并做首次枚举
    void stop();

    QVector<DeviceInfo> devices() const { return m_devices; }
    QString devicesJson() const;
    DeviceInfo deviceById(const QString& id) const;
    void refresh();    // 手动刷新（前端"重新扫描"按钮）

    // 全部可移动盘符（用于排除系统盘/C 盘）
    static QStringList removableRoots();

signals:
    void devicesChanged(const QString& json);
    void devicePlugged(const QString& deviceName);
    void deviceRemoved(const QString& deviceName);
    void toast(const QString& message, const QString& type);

protected:
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void scheduleRefresh();
    void doRefresh();
    QVector<DeviceInfo> enumerateUsb() const;
    QVector<DeviceInfo> enumerateMtp() const;

    // 依据设备名称/型号猜测它支持哪些音频格式（播放器固件通常只吃固定几种）
    static QStringList guessSupportedFormats(const QString& name, bool mtp);

    QVector<DeviceInfo> m_devices;
    QStringList m_knownIds;
    QTimer m_debounce;
    bool m_running = false;
    bool m_filterInstalled = false;
};

} // namespace ms
