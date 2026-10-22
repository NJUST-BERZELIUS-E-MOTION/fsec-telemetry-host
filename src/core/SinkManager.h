#pragma once

#include <QList>
#include <QString>
#include "core/Frame.h"
#include "core/ISink.h"

// =====================================================================
// SinkManager —— 数据处理出口注册表 / 广播器
//
// 持有若干 ISink, pushFrame() 时把同一帧广播给每一个已注册出口。
// 这是"预留数据其它处理方式的接口"的落地处:
//   加一个处理器  =  实现 ISink + registerSink(一行代码)。
//
// 本类不拥有 ISink 的所有权(生命周期由创建方 MainWindow 管理)。
// 本类为普通 C++ 类, 不依赖 Qt 事件循环, 在 GUI 线程内同步分发。
// =====================================================================
class SinkManager {
public:
    SinkManager() = default;
    ~SinkManager() = default;

    // 注册一个数据处理出口(通常在主窗口构造时调用)
    void registerSink(ISink *sink);
    void unregisterSink(ISink *sink);

    // 广播一条帧给所有出口
    void pushFrame(const Frame &frame);

    // 通知所有出口复位内部状态(串口重新打开时)
    void resetAll();

    // 暂停/恢复所有出口的呈现(接收计数照常)
    void setAllPaused(bool paused);

    // 当前已注册的出口列表(只读)
    const QList<ISink *> &sinks() const { return m_sinks; }

private:
    QList<ISink *> m_sinks;
};
