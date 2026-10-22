#include "ui/TelemetryTableModel.h"

#include <QBrush>
#include <QColor>
#include <QJsonValue>

namespace {
QString fmtValue(const FieldSpec *sp, const QJsonValue &v)
{
    if (v.isDouble()) {
        // 接收记录与串口原始 JSON 对齐: 固件用 %.2f 打印, 这里至少保留 2 位
        // 小数(配置里要求更高精度时按配置), 不去尾零, 显示位数不少于原始数据
        int dec = sp ? sp->decimals : 2;
        if (dec < 2)
            dec = 2;
        return QString::number(v.toDouble(), 'f', dec);
    }
    if (v.isString())
        return v.toString();
    if (v.isBool())
        return v.toBool() ? "开" : "关";
    if (v.isNull())
        return "-";
    return v.toString();
}

bool outOfRange(const FieldSpec *sp, const QJsonValue &v)
{
    if (!sp || !sp->hasRange || !v.isDouble())
        return false;
    const double d = v.toDouble();
    return d < sp->minValue || d > sp->maxValue;
}

bool boolOn(const QJsonValue &v)
{
    return v.isBool() ? v.toBool() : (v.isDouble() && v.toDouble() != 0.0);
}

QString headerLabel(const FieldSpec *sp, const QString &key)
{
    if (!sp)
        return key;
    QString text = sp->label;
    if (sp->label != sp->key)
        text += " (" + sp->key + ")";
    if (!sp->unit.isEmpty())
        text += " [" + sp->unit + "]";
    return text;
}
} // namespace

TelemetryTableModel::TelemetryTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int TelemetryTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int TelemetryTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : (2 + m_keys.size());
}

void TelemetryTableModel::resetAll(const QStringList &keys,
                                   const QList<TelemetryRecord> &rows)
{
    beginResetModel();
    m_keys = keys;
    m_rows = rows.toVector();
    while (m_rows.size() > m_maxRows)
        m_rows.removeFirst();
    endResetModel();
}

void TelemetryTableModel::appendRecords(const QList<TelemetryRecord> &rows)
{
    if (rows.isEmpty())
        return;

    beginInsertRows(QModelIndex(), m_rows.size(),
                    m_rows.size() + rows.size() - 1);
    for (const TelemetryRecord &r : rows)
        m_rows.append(r);
    endInsertRows();

    const int excess = m_rows.size() - m_maxRows;
    if (excess > 0) {
        beginRemoveRows(QModelIndex(), 0, excess - 1);
        for (int i = 0; i < excess; ++i)
            m_rows.removeFirst();
        endRemoveRows();
    }
}

QVariant TelemetryTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};

    const TelemetryRecord &rec = m_rows.at(index.row());
    const int col = index.column();

    if (col == 0) {
        if (role == Qt::DisplayRole)
            return QString::number(rec.ordinal);
        if (role == Qt::ToolTipRole)
            return QStringLiteral("样本序号(自打开串口起连续递增)");
        return {};
    }
    if (col == 1) {
        if (role == Qt::DisplayRole)
            return rec.hostTime.toString("HH:mm:ss.zzz");
        if (role == Qt::ToolTipRole)
            return QStringLiteral("PC 接收该样本的时刻");
        return {};
    }

    const QString key = m_keys.at(col - 2);
    const QJsonValue v = rec.fields.value(key);
    const FieldSpec *sp = m_schema ? m_schema->find(key) : nullptr;

    if (role == Qt::DisplayRole) {
        if (v.isUndefined())
            return QString();
        if (sp && sp->isBool) {
            if (v.isNull())
                return QStringLiteral("—");
            return QStringLiteral("●"); // LED 圆点, 颜色见 ForegroundRole
        }
        return fmtValue(sp, v);
    }

    if (role == Qt::ForegroundRole) {
        if (v.isUndefined() || v.isNull())
            return QBrush(QColor(150, 150, 150));
        if (sp && sp->isBool) {
            const bool on = boolOn(v);
            if (sp->activeIsBad) // 1=异常(红), 0=正常(绿)
                return QBrush(on ? QColor(210, 30, 30)
                                 : QColor(0, 150, 50));
            return QBrush(on ? QColor(0, 150, 50) : QColor(210, 30, 30));
        }
        if (outOfRange(sp, v))
            return QBrush(QColor(200, 0, 0));
        return {};
    }

    if (role == Qt::ToolTipRole) {
        if (sp && sp->isBool) {
            if (v.isUndefined() || v.isNull())
                return QStringLiteral("无数据");
            return QStringLiteral("JSON键: %1\n%2")
                .arg(key, boolOn(v) ? QStringLiteral("值=1")
                                    : QStringLiteral("值=0"));
        }
        QString tip = QStringLiteral("JSON键: ") + key;
        if (outOfRange(sp, v) && sp) {
            tip += QStringLiteral("\n越界: 合理范围 [%1, %2]")
                       .arg(sp->minValue)
                       .arg(sp->maxValue);
        }
        return tip;
    }
    return {};
}

QVariant TelemetryTableModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const
{
    if (orientation != Qt::Horizontal)
        return {};

    if (role == Qt::DisplayRole) {
        if (section == 0)
            return QStringLiteral("序号");
        if (section == 1)
            return QStringLiteral("主机时间");
        const QString key = m_keys.value(section - 2);
        const FieldSpec *sp = m_schema ? m_schema->find(key) : nullptr;
        return headerLabel(sp, key);
    }
    if (role == Qt::ToolTipRole) {
        if (section == 0)
            return QStringLiteral(
                "样本序号: 自打开串口(或清空)起连续递增; "
                "burst 一包拆出的多条样本各占一号");
        if (section == 1)
            return QStringLiteral("PC 接收该行的本地时刻");
        const QString key = m_keys.value(section - 2);
        const FieldSpec *sp = m_schema ? m_schema->find(key) : nullptr;
        if (sp)
            return QStringLiteral("JSON键: %1\n单位: %2")
                .arg(key, sp->unit.isEmpty() ? QStringLiteral("(无)")
                                             : sp->unit);
        return QStringLiteral("JSON键: ") + key;
    }
    return {};
}
