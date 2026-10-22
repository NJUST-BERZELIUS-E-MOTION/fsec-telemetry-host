#pragma once

#include <QAbstractTableModel>
#include <QStringList>
#include <QVector>

#include "core/TelemetryRecord.h"
#include "core/TelemetrySchema.h"

// =====================================================================
// TelemetryTableModel —— 接收记录表的数据模型(QTableView 专用)
//
// 为什么不用 QTableWidget: 它每个单元格都是独立 item, 实时插行/滚动/重绘
// 都在 GUI 线程逐项处理, 列多(30+)且行数到 500 时明显拖慢遥测页。
// 用 QAbstractTableModel 后:
//   - 数据只存一份内存(QVector<TelemetryRecord>), 插入为批量 beginInsertRows;
//   - 视图只绘制可见行, 滚动/追加不再重建 item;
//   - 布尔 LED 颜色/越界标红通过 ForegroundRole 提供。
// =====================================================================
class TelemetryTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit TelemetryTableModel(QObject *parent = nullptr);

    void setSchema(const TelemetrySchema *schema) { m_schema = schema; }
    void setMaxRows(int rows) { m_maxRows = rows; }

    // 整体重置: 列(字段 key, 不含固定两列) + 全部行
    void resetAll(const QStringList &keys,
                  const QList<TelemetryRecord> &rows);
    // 追加一批样本(超出上限时从最旧开始裁掉)
    void appendRecords(const QList<TelemetryRecord> &rows);

    const QStringList &keys() const { return m_keys; }

    // ---- QAbstractTableModel ----
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;

private:
    QStringList m_keys;               // 数据列字段 key
    QVector<TelemetryRecord> m_rows;  // 行数据
    const TelemetrySchema *m_schema = nullptr;
    int m_maxRows = 500;
};
