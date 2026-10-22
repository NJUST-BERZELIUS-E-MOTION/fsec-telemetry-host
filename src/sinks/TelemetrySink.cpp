#include "sinks/TelemetrySink.h"

TelemetrySink::TelemetrySink(QObject *parent)
    : QObject(parent)
{
    m_rateTimer.setInterval(1000);
    connect(&m_rateTimer, &QTimer::timeout, this, &TelemetrySink::onRateTick);
    m_rateTimer.start();
}

void TelemetrySink::onFrame(const Frame &frame)
{
    if (!frame.isJson())
        return; // 日志行不属于遥测出口
    if (!frame.json.isObject())
        return; // 顶层数组等非常规对象不做遥测处理

    const QList<TelemetryRecord> records =
        decodeTelemetryFrames(frame);
    if (records.isEmpty())
        return;

    ++m_packets;
    m_records += quint64(records.size());
    m_secondCount += int(records.size());

    if (m_paused)
        return; // 界面暂停: 计数照常, 不向界面发样本

    quint64 base = m_records - quint64(records.size());
    for (int i = 0; i < records.size(); ++i) {
        TelemetryRecord rec = records.at(i);
        rec.ordinal = base + quint64(i) + 1; // 样本序号(自打开起连续)
        emit recordReady(rec);
    }
}

void TelemetrySink::onReset()
{
    m_records = 0;
    m_packets = 0;
    m_secondCount = 0;
    emit countersChanged(0, 0, 0);
}

void TelemetrySink::onRateTick()
{
    emit countersChanged(m_records, m_packets, m_secondCount);
    m_secondCount = 0;
}
