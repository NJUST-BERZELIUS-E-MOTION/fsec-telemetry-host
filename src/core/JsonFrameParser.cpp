#include "core/JsonFrameParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

JsonFrameParser::JsonFrameParser(QObject *parent)
    : QObject(parent)
{
}

void JsonFrameParser::onLineReceived(const QByteArray &line)
{
    Frame frame;
    frame.seq = ++m_lineCount;
    frame.time = QDateTime::currentDateTime();
    frame.raw = line;

    // 尝试整行解析为 JSON
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
    if (error.error == QJsonParseError::NoError && doc.isObject()) {
        frame.kind = FrameKind::Json;
        frame.json = doc.object();
        ++m_jsonCount;
    } else if (error.error == QJsonParseError::NoError && doc.isArray()) {
        frame.kind = FrameKind::Json;
        frame.json = doc.array();
        ++m_jsonCount;
    } else {
        // 不是 JSON: 设备日志 / 或固件下发的其它文本
        frame.kind = FrameKind::Log;
        frame.parseError = error.errorString();
        ++m_logCount;
    }

    emit frameReady(frame);
    emit statsChanged(m_lineCount, m_jsonCount, m_logCount);
}

void JsonFrameParser::reset()
{
    m_lineCount = 0;
    m_jsonCount = 0;
    m_logCount = 0;
    emit statsChanged(0, 0, 0);
}
