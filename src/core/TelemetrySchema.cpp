#include "core/TelemetrySchema.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
// 分组名(与 fields.json 里的 group 一致)
const QString G_PACK = QStringLiteral("包信息");
const QString G_HV   = QStringLiteral("高压/电池");
const QString G_LV   = QStringLiteral("低压");
const QString G_PEDAL= QStringLiteral("踏板");
const QString G_MOT  = QStringLiteral("运动");
const QString G_TC   = QStringLiteral("TC/限制");
const QString G_STAT = QStringLiteral("状态/标志");
const QString G_LORA = QStringLiteral("LoRa");
const QString G_OTHER= QStringLiteral("动态字段");
} // namespace

QList<FieldSpec> TelemetrySchema::defaultFields()
{
    // 顺序 = 界面行/列顺序(包信息 → 高压 → 低压 → 踏板 → 运动 → TC → 状态 → LoRa)
    return {
        // ---- 包信息 ----
        { "rssi",       "信号强度", "dBm", G_PACK, 0, false },
        { "config_id",  "配置ID",   "",    G_PACK, 0, false },
        { "type",       "类型",     "",    G_PACK, 0, false },
        { "ts",         "设备时间", "ms",  G_PACK, 0, false },

        // ---- 高压/电池域 ----
        { "hv_voltage",   "电池箱电压", "V",  G_HV, 0, false },
        { "hv_current",   "电池箱电流", "A",  G_HV, 1, false },
        { "cell_max_v",   "单体最高电压", "V", G_HV, 3, false },
        { "cell_max_t",   "单体最高温度", "℃", G_HV, 1, false },
        { "bms_status",   "BMS状态字", "",    G_HV, 0, false },

        // ---- 低压域 ----
        { "lv_voltage",   "低压电池电压", "V", G_LV, 1, false },

        // ---- 踏板域 ----
        { "accel_ratio",  "加速踏板开度", "%", G_PEDAL, 1, false },
        { "brake_oilpressure", "制动油压", "MPa", G_PEDAL, 1, false },

        // ---- 运动域 ----
        { "wheel_spd_lf", "左前轮线速度", "m/s",  G_MOT, 2, false },
        { "wheel_spd_rf", "右前轮线速度", "m/s",  G_MOT, 2, false },
        { "vehicle_speed","车速",         "m/s",  G_MOT, 2, false },
        { "motor_rps",    "电机轮端角速度","rev/s",G_MOT, 2, false },
        { "motor_speed",  "电机轮端线速度","m/s",  G_MOT, 2, false },
        { "slip_ratio",   "滑移率",       "",     G_MOT, 3, false },
        { "target_torque","目标扭矩",     "N·m",  G_MOT, 0, false },

        // ---- TC/限制域 ----
        { "hv_power_limit","高压功率限制","%", G_TC, 1, false },
        { "slip_limit",    "滑移率限制",  "%", G_TC, 1, false },

        // ---- 状态/标志域 ----
        { "vehicle_status", "车辆状态机",  "", G_STAT, 0, false },
        { "vehicle_flags",  "组合标志字节", "", G_STAT, 0, false },
        { "acc_brk_timer",  "油门刹车冲突计时", "", G_STAT, 0, false },
        { "wss_online",     "轮速传感器在线",  "", G_STAT, 0, true },
        { "battery_online", "电池箱在线",      "", G_STAT, 0, true },
        { "inverter_online","逆变器在线",      "", G_STAT, 0, true },
        { "sc_closed",      "安全接触器闭合",  "", G_STAT, 0, true },
        { "acc_brk_conflict","油门刹车冲突",   "", G_STAT, 0, true },
        { "pedal_unreliable","踏板信号不可靠", "", G_STAT, 0, true },
        { "ready_btn",      "待驶按钮按下",    "", G_STAT, 0, true },
        { "hv_btn",         "上高压按钮按下",  "", G_STAT, 0, true },

        // ---- LoRa 域 ----
        { "lora_tx_fifo_usage", "TxFIFO占用", "包", G_LORA, 0, false },
    };
}

TelemetrySchema::TelemetrySchema(const QString &configPath)
{
    m_fields = defaultFields();
    if (!configPath.isEmpty())
        loadFromJsonFile(configPath);
}

const FieldSpec *TelemetrySchema::find(const QString &key) const
{
    for (const FieldSpec &f : m_fields) {
        if (f.key == key)
            return &f;
    }
    return nullptr;
}

void TelemetrySchema::loadFromJsonFile(const QString &configPath)
{
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly))
        return; // 回退内置默认表

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;

    const QJsonArray arr = doc.object().value("fields").toArray();
    if (arr.isEmpty())
        return;

    QList<FieldSpec> loaded;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        FieldSpec f;
        f.key      = o.value("key").toString();
        f.label    = o.value("label").toString(f.key);
        f.unit     = o.value("unit").toString();
        f.group    = o.value("group").toString(G_OTHER);
        f.decimals = o.value("decimals").toInt(2);
        f.isBool   = o.value("isBool").toBool(false);
        f.activeIsBad = o.value("activeIsBad").toBool(false);
        // 合理范围: min/max 都给出并且 min<max 才启用
        const QJsonValue mn = o.value("min");
        const QJsonValue mx = o.value("max");
        if (mn.isDouble() && mx.isDouble() && mn.toDouble() < mx.toDouble()) {
            f.hasRange = true;
            f.minValue = mn.toDouble();
            f.maxValue = mx.toDouble();
        }
        if (!f.key.isEmpty())
            loaded.append(f);
    }
    if (!loaded.isEmpty())
        m_fields = loaded; // 文件合法且非空才整体替换默认表
}
