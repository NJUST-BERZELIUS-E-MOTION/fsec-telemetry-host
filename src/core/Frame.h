#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonValue>
#include <QString>

// 一"帧" = 串口字节流里切出来的一条完整行(按 \n 切分, 兼容 \r\n / \n)
// 它是上位机流水线里流通的最小数据单元:
//   SerialReceiver(切行) -> JsonFrameParser(分类+解析) -> SinkManager -> ISink们
enum class FrameKind {
    Log, // 普通文本(设备日志), 无法解析为 JSON
    Json // 成功解析为 JSON 对象/数组
};

struct Frame {
    quint64    seq = 0;       // 全局帧序号(本次打开串口起累加)
    QDateTime  time;          // PC 收到该帧的本地时刻
    FrameKind  kind = FrameKind::Log;
    QByteArray raw;           // 原始行文本(不含行尾 \r / \n)
    QJsonValue json;          // kind == Json 时有效
    QString    parseError;    // 解析失败原因(暂未使用, 预留调试)

    bool isJson() const { return kind == FrameKind::Json; }
};
