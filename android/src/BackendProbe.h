// ============================================================================
//  src/BackendProbe.h —— 骨架阶段的后端自检探针
//  证明桌面版后端核心（评分/意图解析/去重）在安卓上原样可用。
//  后续阶段这里会逐步换成真正的 SearchAggregator / DownloadEngine 桥。
// ============================================================================
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

namespace ms {

class BackendProbe : public QObject
{
    Q_OBJECT
    // 自检是否全部通过（绿勾/红叉）
    Q_PROPERTY(bool allOk READ allOk NOTIFY resultChanged)
    // 结果行：{name, value, ok}
    Q_PROPERTY(QVariantList rows READ rows NOTIFY resultChanged)

public:
    explicit BackendProbe(QObject* parent = nullptr);

    bool allOk() const { return m_allOk; }
    QVariantList rows() const { return m_rows; }
    Q_INVOKABLE void run();

signals:
    void resultChanged();

private:
    void addRow(const QString& name, const QString& value, bool ok);

    QVariantList m_rows;
    bool m_allOk = true;
};

} // namespace ms
