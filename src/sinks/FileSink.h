#pragma once

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include "core/Frame.h"
#include "core/ISink.h"
#include "core/TelemetryRecord.h"

// =====================================================================
// FileSink —— 数据处理出口③: 遥测记录(一次记录 = 一个文件夹)
//
// 保存内容 = 拆帧后的每一条样本(一行一条, JSONL), 按帧类型分开, 全部
// 放在同一个记录文件夹里:
//   <文件夹>/burst.jsonl   周期合包(config_id=10, 已逐帧拆开)
//   <文件夹>/data.jsonl    默认单帧(config_id=1/2 等)
//   <文件夹>/event.jsonl   事件流(config_id=20)
//   <文件夹>/other.jsonl   其它/未知类型
//   停止记录时写 <文件夹>/meta.json(会话信息 + 字段定义 + 各类计数)
//
// 每行是"键顺序固定"的紧凑 JSON(手写定序, 非字母序), 便于人工顺读:
//   seq, line_seq, host_time, type, config_id, ts, rssi, <按 schema 顺序的字段>,
//   <未登记的动态字段>
//
// 另提供 exportPretty(): 把本次会话的 JSONL 转成缩进 2 空格的
// <文件夹>/<类型>.pretty.json, 方便给人看/归档。
//
// 记录由"开始记录/停止记录"手动控制; 不响应 setPaused(暂停显示不影响记录)。
// =====================================================================
class FileSink : public QObject, public ISink {
    Q_OBJECT

public:
    explicit FileSink(QObject *parent = nullptr);

    // ---- ISink ----
    QString name() const override { return "FileSink"; }
    void onFrame(const Frame &frame) override;
    // onReset()/setPaused() 使用基类默认实现(记录不被打断)

    // 会话信息(写入 meta)
    void setSessionInfo(const QString &port, int baud);
    // 字段显示顺序(定序输出用; 由 MainWindow 传入 schema 顺序)
    void setFieldOrder(const QStringList &keys);
    // 字段定义(写入 meta, 便于日后分析复现)
    void setFieldDefs(const QJsonArray &defs);

    bool isRecording() const { return m_active; }
    QString basePath() const { return m_base; }
    quint64 writtenCount() const { return m_written; }

    // 本次会话是否已经产生过记录文件(用于"导出可读JSON"是否可用)
    bool hasSessionFiles() const;
    // 从当前会话的 JSONL 生成缩进版 .pretty.json; 失败时 err 给出原因
    bool exportPretty(QString *err) const;

public slots:
    // 开始记录到"记录文件夹"basePath(可带 .jsonl/.json 后缀, 内部会去掉);
    // 文件夹不存在会自动创建; 同名文件已存在则追加。返回是否成功。
    bool start(const QString &basePathOrFile);
    void stop();

signals:
    void recordChanged(bool active, quint64 written);
    void recordError(const QString &message);

private:
    QString typeKeyOf(const QString &type) const;      // burst/data/event/other
    QString fileNameForType(const QString &typeKey) const;
    QFile *fileForType(const QString &typeKey);        // 懒打开(追加)
    QByteArray buildLine(const TelemetryRecord &rec,
                         quint64 sampleSeq) const;      // 定序拼接
    void writeMeta();
    static QByteArray literal(const QJsonValue &v);    // 单个 JSON 字面量

    bool          m_active = false;
    QString       m_base;                 // 去掉后缀的基名(绝对路径)
    QString       m_port;
    int           m_baud = 0;
    QJsonArray    m_defs;                 // 字段定义(meta)
    QStringList   m_order;                // 字段输出顺序
    QHash<QString, QFile *> m_files;      // typeKey -> 文件
    QHash<QString, quint64> m_countByType;// typeKey -> 条数
    quint64       m_written = 0;          // 本次会话总条数
    quint64       m_sampleSeq = 0;        // 本次记录会话样本序号(写入 seq)
    QDateTime     m_started;
    QDateTime     m_ended;
};
