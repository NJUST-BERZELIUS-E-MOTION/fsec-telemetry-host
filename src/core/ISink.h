#pragma once

#include <QString>
#include "core/Frame.h"

// =====================================================================
// ISink —— 数据"其它处理方式"的统一接口(本工程的扩展点)
//
// 数据流: 每条 Frame 会依次交给所有注册到 SinkManager 的 ISink。
// 首版只注册两个界面出口(原始日志 / 遥测表格), 未来要加新处理方式时:
//
//   1. 新建 class MySink : public QObject, public ISink { ... };
//      onFrame() 里写你的处理逻辑(如写文件、统计、绘图、转发)。
//   2. 在 MainWindow 里 new MySink(...) 并 registerSink(sink)。
//
// 流水线本身不用改任何代码。
// =====================================================================
class ISink {
public:
    virtual ~ISink() = default;

    // 处理器名字, 便于日志/调试
    virtual QString name() const = 0;

    // 数据入口: 收到一条完整帧(必须实现)
    virtual void onFrame(const Frame &frame) = 0;

    // 可选: 串口重开 / 用户点"清空"时复位内部状态
    virtual void onReset() {}

    // 可选: 暂停/恢复输出(接收照常进行, 只是本处理器不再对外呈现)
    virtual void setPaused(bool paused) { Q_UNUSED(paused); }
};
