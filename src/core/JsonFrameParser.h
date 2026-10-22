#pragma once

#include <QObject>

#include "core/Frame.h"

// =====================================================================
// JsonFrameParser —— 解析层(行 -> 帧)
//
// 收到 SerialReceiver 切好的一行文本后:
//   1. 盖 PC 时间戳 + 全局帧序号;
//   2. 尝试用 QJsonDocument 解析为 JSON;
//      成功  -> FrameKind::Json(可扩展出 json 里带的内容);
//      失败  -> FrameKind::Log(设备日志, 原样呈现);
//   3. 产出完整 Frame 并发出 frameReady(), 交给 SinkManager 分发。
//
// 为什么按"行"试解析 JSON?
//   固件端(Telemetry.c)收到 LoRa 数据包后 printf 输出完整 JSON 并以
//   \r\n 结尾, 而 Log_Write 调试日志以 \n 结尾 —— 两条流混在同一条
//   串口上, 所以必须"逐行尝试解析, 解析不了就当日志"。
// =====================================================================
class JsonFrameParser : public QObject {
    Q_OBJECT

public:
    explicit JsonFrameParser(QObject *parent = nullptr);

    quint64 lineCount() const { return m_lineCount; }
    quint64 jsonCount() const { return m_jsonCount; }
    quint64 logCount() const { return m_logCount; }

public slots:
    void onLineReceived(const QByteArray &line);
    void reset(); // 串口重开时清零计数

signals:
    // 一条解析好的帧
    void frameReady(const Frame &frame);

    // 统计变化(总行数 / JSON 行数 / 日志行数)
    void statsChanged(quint64 lines, quint64 jsonLines, quint64 logLines);

private:
    quint64 m_lineCount = 0;
    quint64 m_jsonCount = 0;
    quint64 m_logCount = 0;
};
