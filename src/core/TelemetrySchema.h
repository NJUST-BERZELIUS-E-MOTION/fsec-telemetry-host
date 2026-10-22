#pragma once

#include <QList>
#include <QString>

// =====================================================================
// TelemetrySchema —— 遥测字段显示/选择配置
//
// 固件 Telemetry.c 的 JSON 键(21 个传感器 + 包信息 + 状态标志展开),
// 在这里映射成界面友好的: 中文名 / 单位 / 小数位 / 分组。
// 分组用于"字段选择"面板按域归类; 顺序决定最新值表行序与记录表列序。
//
// 数据来源(优先级):
//   1. exe 旁 config/fields.json  —— 界面显示与分组改这里即可;
//   2. 代码内置默认表(与固件描述符表一一对应), 文件缺失也能跑。
//
// 说明: 这不是协议解析配置, 只影响"怎么显示/怎么选"。
// 未在表里的键会在运行时自动发现并归入"动态字段"组。
// =====================================================================

struct FieldSpec {
    QString key;         // JSON 字段名(与固件一致)
    QString label;       // 界面显示名(中文)
    QString unit;        // 单位
    QString group;       // 域分组(字段选择面板的顶层分组)
    int     decimals = 2; // 数字保留小数位
    bool    isBool = false; // 0/1 按 LED 显示
    // 布尔语义: false=1正常(在线/闭合/按下等), true=1异常(冲突/不可靠)
    bool    activeIsBad = false;
    // 合理范围(用于异常检测/曲线剔除; min/max 都配置才启用)
    bool    hasRange = false;
    double  minValue = 0.0;
    double  maxValue = 0.0;
};

class TelemetrySchema {
public:
    // 载入 configPath 的 JSON; 失败/缺文件自动回退内置默认表
    explicit TelemetrySchema(const QString &configPath = QString());

    // 全量有序字段表(即 m_fields)
    const QList<FieldSpec> &fields() const { return m_fields; }

    // 按 JSON 键查询显示规格; 找不到返回 nullptr
    const FieldSpec *find(const QString &key) const;

    // 内置默认表: 与固件 Telemetry.c 的 Sensor_Table / 输出 JSON 一致
    static QList<FieldSpec> defaultFields();

private:
    void loadFromJsonFile(const QString &configPath);

    QList<FieldSpec> m_fields;
};
