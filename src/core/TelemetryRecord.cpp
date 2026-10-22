#include "core/TelemetryRecord.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QVector>
#include <algorithm>
#include <limits>

// =====================================================================
// decodeTelemetryObject —— 核心拆帧逻辑
// =====================================================================
QList<TelemetryRecord> decodeTelemetryObject(const QJsonObject &obj,
                                             quint64 lineSeq,
                                             const QDateTime &hostTime)
{
    QList<TelemetryRecord> out;

    // 元数据(直接来自行级对象)
    const QString type = obj.value("type").toString();

    // ---- burst 合包: frames 数组 -> 逐帧拆开 ----
    if (type == "burst") {
        const QJsonArray frames = obj.value("frames").toArray();
        QJsonObject base = obj; // 行级(去掉 frames 容器)
        base.remove("frames");
        base.remove("ts"); // 帧级 ts 才是各样本的时间

        // 先按帧内设备时间 ts 升序归一化: 不管发送端如何装包,
        // 同一 burst 拆出的样本序号/记录顺序始终与设备时间同向
        QVector<QPair<double, QJsonObject>> sorted;
        for (const QJsonValue &fv : frames) {
            const QJsonObject fo = fv.toObject();
            if (fo.isEmpty())
                continue;
            const QJsonValue tv = fo.value("ts");
            const double t = tv.isDouble()
                                 ? tv.toDouble()
                                 : std::numeric_limits<double>::infinity();
            sorted.append(qMakePair(t, fo));
        }
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const QPair<double, QJsonObject> &a,
                            const QPair<double, QJsonObject> &b) {
                             return a.first < b.first;
                         });

        for (const auto &item : sorted) {
            TelemetryRecord rec;
            rec.lineSeq = lineSeq;
            rec.hostTime = hostTime;
            rec.fields = base; // type/config_id/rssi...
            // 帧级字段(ts + 通道数据等)覆盖进来
            const QJsonObject &fo = item.second;
            for (auto it = fo.constBegin(); it != fo.constEnd(); ++it)
                rec.fields.insert(it.key(), it.value());
            out.append(rec);
        }
        return out;
    }

    // ---- 其余( event/data/其它 JSON 对象 ): 一行 = 一条样本 ----
    if (!obj.isEmpty()) {
        TelemetryRecord rec;
        rec.lineSeq = lineSeq;
        rec.hostTime = hostTime;
        rec.fields = obj;
        out.append(rec);
    }
    return out;
}

QList<TelemetryRecord> decodeTelemetryFrames(const Frame &frame)
{
    if (!frame.isJson())
        return {};
    if (!frame.json.isObject())
        return {};
    QList<TelemetryRecord> recs =
        decodeTelemetryObject(frame.json.toObject(), frame.seq, frame.time);
    for (TelemetryRecord &r : recs)
        r.rawLine = frame.raw;
    return recs;
}

bool isRecordMetaKey(const QString &key)
{
    // 这些键由解码器/视图专门处理, 不作为"传感器字段"自动发现
    return key == "type" || key == "rssi" || key == "config_id" ||
           key == "ts" || key == "frames" || key == "seq" ||
           key == "line_seq" || key == "host_time";
}
