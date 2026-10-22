#pragma once

#include <QJsonValue>
#include <QString>

// =====================================================================
// IPostProcessor —— 数据"后处理"预留接口(尚未实现, 仅留扩展点)
//
// 用途: 曲线页"处理模式"目前只有"原始"; 将来若要对遥测值做滤波/换算/
// 派生量(如按 scale 换算、滑动平均、差分、滑移率合成等), 实现本接口并
// 在 MainWindow 的曲线数据入口处接上即可:
//
//   1. class MyFilter : public IPostProcessor {
//          QString name() const override { return "低通滤波"; }
//          QJsonValue process(const QString &key, const QJsonValue &raw) const override;
//      };
//   2. MainWindow 增加处理器实例并在取 Y 值时调用 process(key, raw)。
//
// 流水线本身(SerialReceiver → JsonFrameParser → 拆帧 → 界面)无需改动。
// =====================================================================
class IPostProcessor {
public:
    virtual ~IPostProcessor() = default;

    // 处理器名称(曲线页"处理模式"下拉里显示)
    virtual QString name() const = 0;

    // 对某字段的原始样本值做后处理, 返回用于绘制的值。
    // key 为 JSON 字段名; raw 为该样本的原始值。
    virtual QJsonValue process(const QString &key,
                               const QJsonValue &raw) const = 0;
};
