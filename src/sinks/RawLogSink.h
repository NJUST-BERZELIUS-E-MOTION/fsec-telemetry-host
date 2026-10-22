#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

#include "core/Frame.h"
#include "core/ISink.h"

// =====================================================================
// RawLogSink —— 数据处理出口①: 原始数据窗口
//
// 把每一条帧(无论 JSON 还是日志文本)原样转发给界面"原始数据"页签。
// 界面上按类型着色显示: JSON 一条一色、日志一色、系统消息一色。
//
// 只做"转发给视图"一件事; 真正要不要附加时间戳、颜色等由视图决定。
// =====================================================================
class RawLogSink : public QObject, public ISink {
    Q_OBJECT

public:
    explicit RawLogSink(QObject *parent = nullptr);

    // ---- ISink ----
    QString name() const override { return "RawLogSink"; }
    void onFrame(const Frame &frame) override;
    void setPaused(bool paused) override { m_paused = paused; }

signals:
    // 一条帧送达视图: seq=帧序号, time=主机接收时刻,
    // isJson=是否 JSON, text=原始行文本
    void lineAppended(quint64 seq, const QDateTime &time, bool isJson,
                      const QString &text);

private:
    bool m_paused = false;
};
