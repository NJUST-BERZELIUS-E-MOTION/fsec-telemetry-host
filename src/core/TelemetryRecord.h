#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

#include "core/Frame.h"

// =====================================================================
// TelemetryRecord —— 拆帧后的最小显示/存储单元(一条样本)
//
// 固件新协议下, 串口一行 JSON 可能包含多种内容:
//   {"type":"event",...}                       -> 1 条样本(单对象)
//   {"type":"data","config_id":n,...}          -> 1 条样本(单对象)
//   {"type":"burst","frames":[{..},{..},..]}   -> N 条样本(逐帧拆开)
//   {"type":"data","payload":".."}             -> 1 条样本(hex 兜底)
//
// 本文件的解码函数负责把"一行 JSON"展开成 0..N 条 TelemetryRecord,
// 每条 record.fields 是完整平铺字段(type/rssi/config_id/ts/各传感器),
// 界面表格、最新值、文件保存全部以 record 为单位 —— 与用户所见的
// "记录表一行"一致。
// =====================================================================
struct TelemetryRecord {
    quint64   ordinal = 0;   // 样本序号(自打开串口起连续递增; burst 拆出的每条各占一号)
    quint64   lineSeq = 0;   // 源 JSON 行的全局序号(同一 burst 的多条样本相同)
    QDateTime hostTime;      // PC 接收该行的本地时刻
    QJsonObject fields;      // 平铺字段(含 type/rssi/config_id/ts/传感器键值)
    QByteArray rawLine;      // 源串口行原文(异常留证/排查用)
};

// 把一个已解析为 JSON 对象的帧解码成样本列表(见文件头注释)
QList<TelemetryRecord> decodeTelemetryFrames(const Frame &frame);

// 对象版入口: 便于复用(解码纯函数, 不依赖 Frame)
QList<TelemetryRecord> decodeTelemetryObject(const QJsonObject &obj,
                                             quint64 lineSeq,
                                             const QDateTime &hostTime);

// 判断某键是否为解码器保留的元数据键(不入传感器/显示自动发现流程)
bool isRecordMetaKey(const QString &key);
