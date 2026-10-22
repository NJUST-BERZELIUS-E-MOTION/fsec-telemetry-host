#include "core/SinkManager.h"

void SinkManager::registerSink(ISink *sink)
{
    if (sink && !m_sinks.contains(sink))
        m_sinks.append(sink);
}

void SinkManager::unregisterSink(ISink *sink)
{
    m_sinks.removeAll(sink);
}

void SinkManager::pushFrame(const Frame &frame)
{
    for (ISink *sink : m_sinks) {
        if (sink)
            sink->onFrame(frame);
    }
}

void SinkManager::resetAll()
{
    for (ISink *sink : m_sinks) {
        if (sink)
            sink->onReset();
    }
}

void SinkManager::setAllPaused(bool paused)
{
    for (ISink *sink : m_sinks) {
        if (sink)
            sink->setPaused(paused);
    }
}
