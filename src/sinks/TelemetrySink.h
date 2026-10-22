#pragma once

#include <QObject>
#include <QTimer>

#include "core/Frame.h"
#include "core/ISink.h"
#include "core/TelemetryRecord.h"

// =====================================================================
// TelemetrySink —— 数据处理出口②: 遥测(结构化)视图的数据源
//
// 把"解析成功且为 JSON 对象"的每一行交给拆帧解码器
// (decodeTelemetryFrames: burst 逐帧拆开 / event / data / hex 兜底),
// 得到 0..N 条样本记录, 逐条发出 recordReady(record), 界面据此刷新
// 最新值表与记录表 —— 因此界面所见"一行"永远是一条样本。
//
// 计数按样本与包分别统计, 每秒发一次给界面。
// 未解析为 JSON 的日志行不属于本出口(RawLogSink 负责原样显示)。
// =====================================================================
class TelemetrySink : public QObject, public ISink {
    Q_OBJECT

public:
    explicit TelemetrySink(QObject *parent = nullptr);

    // ---- ISink ----
    QString name() const override { return "TelemetrySink"; }
    void onFrame(const Frame &frame) override;
    void onReset() override;
    void setPaused(bool paused) override { m_paused = paused; }

    quint64 recordCount() const { return m_records; }
    quint64 packetCount() const { return m_packets; }

signals:
    // 一条拆好的样本(界面据此刷新)
    void recordReady(const TelemetryRecord &record);

    // 每秒一次: 累计样本数 / 累计包数 / 上一秒样本速率
    void countersChanged(quint64 records, quint64 packets, int recordsPerSec);

private:
    void onRateTick();

    bool     m_paused = false;
    quint64  m_records = 0;   // 累计样本(记录表行数)
    quint64  m_packets = 0;   // 累计 JSON 包(串口行)
    int      m_secondCount = 0;
    QTimer   m_rateTimer;
};
