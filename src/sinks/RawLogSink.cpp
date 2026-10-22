#include "sinks/RawLogSink.h"

RawLogSink::RawLogSink(QObject *parent)
    : QObject(parent)
{
}

void RawLogSink::onFrame(const Frame &frame)
{
    if (m_paused)
        return;

    // 原样转发(不含行尾)
    emit lineAppended(frame.seq,
                      frame.time,
                      frame.kind == FrameKind::Json,
                      QString::fromUtf8(frame.raw));
}
