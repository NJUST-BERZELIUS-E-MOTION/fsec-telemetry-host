#include "sinks/FileSink.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

FileSink::FileSink(QObject *parent)
    : QObject(parent)
{
}

// ---------------------------------------------------------------------
// 会话信息 / 字段定义
// ---------------------------------------------------------------------
void FileSink::setSessionInfo(const QString &port, int baud)
{
    m_port = port;
    m_baud = baud;
}

void FileSink::setFieldOrder(const QStringList &keys)
{
    m_order = keys;
}

void FileSink::setFieldDefs(const QJsonArray &defs)
{
    m_defs = defs;
}

// ---------------------------------------------------------------------
// 类型 -> 文件
// ---------------------------------------------------------------------
QString FileSink::typeKeyOf(const QString &type) const
{
    if (type == "burst")
        return "burst";
    if (type == "data")
        return "data";
    if (type == "event")
        return "event";
    return "other";
}

QString FileSink::fileNameForType(const QString &typeKey) const
{
    return m_base + "/" + typeKey + ".jsonl";
}

QFile *FileSink::fileForType(const QString &typeKey)
{
    if (!m_active)
        return nullptr;
    if (QFile *f = m_files.value(typeKey, nullptr))
        return f;

    const QString path = fileNameForType(typeKey);
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        emit recordError("无法创建记录目录: " + info.absolutePath());
        return nullptr;
    }
    auto *f = new QFile(path, this);
    if (!f->open(QIODevice::WriteOnly | QIODevice::Append)) {
        emit recordError("无法打开记录文件 " + path + " : " + f->errorString());
        delete f;
        return nullptr;
    }
    m_files.insert(typeKey, f);
    // 首条写入时给出提示(哪个文件开始记了)
    return f;
}

// ---------------------------------------------------------------------
// 单个 JSON 字面量(借助 QJsonDocument 保证转义正确; 定序拼装由 buildLine 完成)
// ---------------------------------------------------------------------
QByteArray FileSink::literal(const QJsonValue &v)
{
    const QJsonArray one{ v };
    const QByteArray arr =
        QJsonDocument(one).toJson(QJsonDocument::Compact); // [ value ]
    if (arr.size() < 2)
        return QByteArray("null");
    return arr.mid(1, arr.size() - 2); // 去掉方括号
}

// ---------------------------------------------------------------------
// 一行样本: 键顺序固定(非字母序)
//   seq, line_seq, host_time, type, config_id, ts, rssi, <schema 顺序字段>, <其它>
// ---------------------------------------------------------------------
QByteArray FileSink::buildLine(const TelemetryRecord &rec,
                               quint64 sampleSeq) const
{
    QByteArray out;
    out += '{';
    bool first = true;
    QSet<QString> written;

    // 按指定顺序写出键值(重复键/缺省值自动跳过, 避免写出 null 噪声)
    auto emitKV = [&](const QString &key, const QJsonValue &v) {
        if (key.isEmpty() || written.contains(key) || v.isUndefined())
            return;
        written.insert(key);
        if (!first)
            out += ',';
        first = false;
        out += '"';
        out += key.toUtf8();
        out += "\":";
        out += literal(v);
    };

    emitKV("seq", QJsonValue(double(sampleSeq)));
    emitKV("line_seq", QJsonValue(double(rec.lineSeq)));
    emitKV("host_time",
           QJsonValue(rec.hostTime.toString("yyyy-MM-dd HH:mm:ss.zzz")));
    emitKV("type", rec.fields.value("type"));
    emitKV("config_id", rec.fields.value("config_id"));
    emitKV("ts", rec.fields.value("ts"));
    emitKV("rssi", rec.fields.value("rssi"));

    // 按 schema 顺序输出其余字段
    for (const QString &key : m_order)
        emitKV(key, rec.fields.value(key));

    // 动态新增(未登记)字段: 按字母序补在末尾
    QStringList rest;
    for (auto it = rec.fields.constBegin(); it != rec.fields.constEnd(); ++it) {
        if (!written.contains(it.key()))
            rest << it.key();
    }
    rest.sort();
    for (const QString &key : rest)
        emitKV(key, rec.fields.value(key));

    out += '}';
    return out;
}

// ---------------------------------------------------------------------
// 开始 / 停止
// ---------------------------------------------------------------------
bool FileSink::start(const QString &basePathOrFile)
{
    if (basePathOrFile.isEmpty())
        return false;

    stop(); // 若已在记录, 先收尾(并写上一会话的 meta)

    QString base = QDir::fromNativeSeparators(basePathOrFile);
    if (base.endsWith(".jsonl", Qt::CaseInsensitive))
        base.chop(6);
    else if (base.endsWith(".json", Qt::CaseInsensitive))
        base.chop(5);

    // 一次记录 = 一个文件夹: <基名>/ 下放 burst/data/event/other.jsonl + meta.json
    if (!QDir().mkpath(base)) {
        emit recordError("无法创建记录文件夹: " + base);
        return false;
    }

    m_base = base;
    m_files.clear();
    m_countByType.clear();
    m_written = 0;
    m_sampleSeq = 0; // 本记录会话的样本序号从 0 起(写入 seq 时先自增)
    m_started = QDateTime::currentDateTime();
    m_ended = QDateTime();
    m_active = true; // 文件按类型懒打开(收到该类型帧时才建)

    emit recordChanged(true, 0);
    return true;
}

void FileSink::stop()
{
    if (!m_active && m_files.isEmpty()) {
        emit recordChanged(false, m_written);
        return;
    }

    for (QFile *f : m_files) {
        if (f) {
            f->flush();
            f->close();
            f->deleteLater();
        }
    }
    m_files.clear();

    if (m_active) {
        m_ended = QDateTime::currentDateTime();
        writeMeta(); // 会话结束落 meta
    }
    m_active = false;
    emit recordChanged(false, m_written);
}

// ---------------------------------------------------------------------
// 写入
// ---------------------------------------------------------------------
void FileSink::onFrame(const Frame &frame)
{
    if (!m_active)
        return;
    if (!frame.isJson())
        return; // 非 JSON 日志不进记录文件

    const QList<TelemetryRecord> records = decodeTelemetryFrames(frame);
    if (records.isEmpty())
        return;

    for (const TelemetryRecord &rec : records) {
        const QString type = rec.fields.value("type").toString();
        const QString key = typeKeyOf(type);
        QFile *f = fileForType(key);
        if (!f)
            continue; // 打开失败已报错, 跳过该条

        ++m_sampleSeq; // 文件内样本序号(每行唯一, 供导入/绘图使用)
        f->write(buildLine(rec, m_sampleSeq));
        f->write("\n");
        f->flush();

        ++m_written;
        ++m_countByType[key];
    }
    emit recordChanged(true, m_written);
}

// ---------------------------------------------------------------------
// meta 文件
// ---------------------------------------------------------------------
void FileSink::writeMeta()
{
    if (m_base.isEmpty())
        return;

    QJsonObject counts;
    for (auto it = m_countByType.constBegin(); it != m_countByType.constEnd();
         ++it)
        counts.insert(it.key(), double(it.value()));

    QJsonObject meta;
    meta.insert("base", m_base);
    meta.insert("serial_port", m_port);
    meta.insert("baud", m_baud);
    meta.insert("started", m_started.toString("yyyy-MM-dd HH:mm:ss.zzz"));
    meta.insert("ended", m_ended.isValid()
                             ? m_ended.toString("yyyy-MM-dd HH:mm:ss.zzz")
                             : QString());
    meta.insert("total_samples", double(m_written));
    meta.insert("counts_by_type", counts);
    meta.insert("note",
                "burst/data/event/other 分文件存放, 一行一条样本(JSONL); "
                "键顺序固定; 可用\"导出可读JSON\"生成缩进版");
    meta.insert("fields", m_defs);

    QFile f(m_base + "/meta.json");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit recordError("无法写入 meta 文件: " + f.fileName() + " : " +
                         f.errorString());
        return;
    }
    f.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
    f.close();
}

// ---------------------------------------------------------------------
// 可读 JSON 导出 / 会话文件检查
// ---------------------------------------------------------------------
bool FileSink::hasSessionFiles() const
{
    if (m_base.isEmpty())
        return false;
    static const QStringList keys = { "burst", "data", "event", "other" };
    for (const QString &k : keys) {
        if (QFileInfo::exists(m_base + "/" + k + ".jsonl"))
            return true;
    }
    return false;
}

bool FileSink::exportPretty(QString *err) const
{
    if (m_base.isEmpty()) {
        if (err)
            *err = "尚未设置记录基名";
        return false;
    }

    // 正在记录时先落盘, 保证导出内容最新
    for (QFile *f : m_files) {
        if (f && f->isOpen())
            f->flush();
    }

    static const QStringList keys = { "burst", "data", "event", "other" };
    int exported = 0;
    for (const QString &k : keys) {
        const QString src = m_base + "/" + k + ".jsonl";
        QFile in(src);
        if (!in.exists())
            continue;
        if (!in.open(QIODevice::ReadOnly)) {
            if (err)
                *err = "无法读取 " + src;
            return false;
        }

        QJsonArray arr;
        while (!in.atEnd()) {
            const QByteArray line = in.readLine().trimmed();
            if (line.isEmpty())
                continue;
            const QJsonDocument d = QJsonDocument::fromJson(line);
            if (d.isObject())
                arr.append(d.object()); // 解析不了的坏行跳过
        }
        in.close();

        QFile out(m_base + "/" + k + ".pretty.json");
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (err)
                *err = "无法写入 " + out.fileName() + " : " + out.errorString();
            return false;
        }
        out.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
        out.close();
        ++exported;
    }

    if (exported == 0) {
        if (err)
            *err = "没有找到本次会话的记录文件(还没开始记录或未收到数据)";
        return false;
    }
    return true;
}
