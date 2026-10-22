#pragma once

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMainWindow>
#include <QPair>
#include <QPointF>
#include <QSet>
#include <QStringList>
#include <QVector>

#include "core/TelemetryRecord.h"
#include "core/TelemetrySchema.h"

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;
class QTableView;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QChartView;
class QChart;
class QLineSeries;
class QValueAxis;
class QTimer;
class QStackedWidget;
class QScrollArea;
class QVBoxLayout;
class QAbstractAxis;

class SerialReceiver;
class JsonFrameParser;
class SinkManager;
class FileSink;
class RawLogSink;
class TelemetrySink;
class TelemetryTableModel;

// 一阶低通滤波器状态(每条曲线各自独立)
struct PlotFilterState {
    double y = 0.0;      // 滤波输出(上一次)
    double tMs = -1.0;   // 上一时刻(ms)
    bool   valid = false;
};

// =====================================================================
// MainWindow —— 主窗口(纯代码布局)
//
// 流水线: 串口 → 行切分 → JSON 解析(Frame) → SinkManager → 各 ISink。
//
// "遥测数据"页签的显示模型(按新固件多帧协议):
//   串口一行 JSON  →(TelemetrySink 拆帧)→ 0..N 条样本记录(record)
//   界面按 record 逐条呈现, 字段集合完全由接收内容决定:
//     - 已知字段按 config/fields.json 的名称/单位/分组显示;
//     - 未知字段首次出现自动注册(归入"动态字段")并出现在界面上;
//     - 左侧"字段选择"面板按分组勾选要看的字段(选择即时生效并记忆)。
//   最新值表只显示勾选字段的最新样本; 记录表每样本一行。
// =====================================================================
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    // ---- 串口控制 ----
    void refreshPorts();
    void onOpenToggled(bool checked);
    void onPortError(const QString &message);
    void onConnectionOpened();
    void onConnectionClosed();

    // ---- 数据显示 ----
    void appendRawLine(quint64 seq, const QDateTime &time, bool isJson,
                       const QString &text);
    void onParserStats(quint64 lines, quint64 jsonLines, quint64 logLines);
    void onTelemetryRecord(const TelemetryRecord &record);
    void onTelemetryCounters(quint64 records, quint64 packets, int recordsPerSec);

    // ---- 视图操作 ----
    void clearRawView();
    void clearTelemetryView();
    void onPauseToggled(bool checked);

    // ---- 字段选择 ----
    void onFieldItemChanged(QTreeWidgetItem *item, int column);
    void selectAllFields();
    void selectNoFields();

    // ---- 状态 LED 常显面板 ----
    void updateLedPanel();

    // ---- 曲线页 ----
    void onPlotFieldChanged(QListWidgetItem *item);
    void onPlotXChanged(int index);
    void onPlotModeChanged(int index);
    void onPlotLayoutChanged(int index);
    void onPlotScaleChanged(int index);
    void clearPlot();
    void resetPlotZoom();  // 退出框选缩放, 恢复自动量程
    void onUiTick(); // 定时批量上屏(最新值/记录/曲线), 根治逐样本刷新卡顿

    // ---- 文件记录 ----
    void chooseRecordFile();
    void onRecordToggled(bool checked);
    void onRecordChanged(bool active, quint64 written);
    void onRecordError(const QString &message);
    void exportReadableJson(); // 导出缩进版可读 JSON
    void exportVbo();          // 导出 VBOX(.vbo) 文本数据文件
    void importRecords();      // 导入之前保存的记录文件回看

private:
    // ---- 视图构建 ----
    QWidget *buildControlBar();
    QWidget *buildRecordBar();
    QWidget *buildRawTab();
    QWidget *buildTelemetryTab();
    QWidget *buildFieldPanel();
    QWidget *buildLedPanel();
    QWidget *buildPlotTab();
    void populatePlotFields();                  // 曲线页: 填充可选 Y 字段
    void populatePlotRangeFields();             // 曲线页: 显示范围过滤的字段下拉
    double plotX(const TelemetryRecord &rec); // X 轴取值(序号/ts/相对秒; ts 需去回卷)
    double m_tsLast = -1.0;   // 时间轴去回卷用: 上一个 ts(已加偏移)
    double m_tsOffset = 0.0;  // 24 位 ts 回卷累计偏移
    double recordTimeMs(const TelemetryRecord &rec) const; // 滤波用时间(ms)
    bool lowpassEnabled() const;                // 是否启用一阶低通
    void resetPlotFilters();                    // 清空滤波状态(重建/切模式时)
    double plotValueFor(const QString &key, const TelemetryRecord &rec); // 原始或滤波值
    void rebuildPlotFromHistory();              // 曲线页: 用曲线缓冲重建
    // 曲线按"连续有效段"绘制: 越界/缺失处断开, 不画跨坏点的连线
    struct PlotSeriesGroup {
        QVector<QLineSeries *> segments; // 连续有效段(每段一条 QLineSeries)
        QLineSeries *legendSeries = nullptr; // 仅用于图例的空曲线(同图模式)
        bool broken = true;              // 上一个样本无效, 下一个有效点开新段
    };
    QLineSeries *newPlotSegment(const QString &key, QChart *chart,
                                QValueAxis *xAxis, QValueAxis *yAxis,
                                QWidget *hoverOwner);
    void trimPlotGroup(PlotSeriesGroup &g, int maxPts);
    void appendPlotPoint(const TelemetryRecord &rec); // (占位, 实际批量描点)
    void updatePlotAxes();                      // 按现有数据自适应坐标范围
    // 悬停数值提示: 显示 X(序号/ts/相对秒)、显示值; 低通时附原始值
    void showPlotPointTooltip(const QString &key, const QPointF &pt,
                              QWidget *owner);
    double rawValueAt(const QString &key, double targetX) const;

    // ---- 字段注册 / 可见性 ----
    void registerField(const QString &key, const FieldSpec *spec);
    void loadHiddenFields();
    void saveHiddenFields() const;
    QStringList orderedVisibleKeys() const; // 按注册顺序过滤出可见字段

    // ---- 自适应显示 ----
    void refreshLatestValues();     // 按可见字段重建最新值表
    void rebuildHistoryTable();     // 按可见字段重建接收记录(QTableView 模型)
    void updateLatestFromRecord(const TelemetryRecord &rec);

    // ---- 工具 ----
    void appendSystemLine(const QString &text);
    void appendRawHtml(const QString &html);
    void flushRawPending();              // 批量刷新原始数据页(20ms 一次)
    void flushPendingToCurveBuffer();    // 把待上屏样本补进曲线缓冲
    bool telemetryTabVisible() const;    // 遥测页是否在前台(隐藏页不刷新)
    bool curveTabVisible() const;        // 曲线页是否在前台
    QString headerForField(const FieldSpec &f) const; // "label (key) [unit]"
    static QString formatValue(const FieldSpec *spec, const QJsonValue &v);
    // 异常检测/留证
    static bool valueOutOfRange(const FieldSpec *spec, const QJsonValue &v);
    static void applyRangeStyle(QTableWidgetItem *item, const FieldSpec *spec,
                                const QJsonValue &v);
    void logAnomaly(const QString &reason, const QByteArray &raw,
                    const QString &detail);
    void updateAnomalyLabel();
    void updateRecordControls();
    QJsonArray buildFieldDefsJson() const; // 字段定义(写入记录 meta)
    // 读取一个记录文件(JSONL / 缩进JSON数组 / 单对象), 成功返回 true
    bool loadRecordsFromFile(const QString &path, QList<QJsonObject> *out,
                             QString *err) const;
    void applyImportedRecords(const QList<QJsonObject> &objs); // 灌入显示缓存
    // VBO 导出辅助: 收集本次会话全部样本 / 写 .vbo
    bool gatherExportRecords(QList<TelemetryRecord> *out, QString *err) const;
    bool writeVboFile(const QList<TelemetryRecord> &records, const QString &path,
                      QString *err) const;
    QString defaultRecordDir() const;
    QString suggestedRecordPath() const;

    // ---- 流水线对象 ----
    SerialReceiver  *m_receiver = nullptr;
    JsonFrameParser *m_parser = nullptr;
    SinkManager     *m_sinks = nullptr;
    FileSink        *m_fileSink = nullptr;
    RawLogSink      *m_rawSink = nullptr;
    TelemetrySink   *m_telemetrySink = nullptr;

    TelemetrySchema m_schema; // 字段显示配置(内置默认 + config/fields.json)

    // ---- 控制条 ----
    QComboBox  *m_portCombo = nullptr;
    QComboBox  *m_baudCombo = nullptr;
    QPushButton *m_openBtn = nullptr;
    QCheckBox  *m_pauseChk = nullptr;

    // ---- 原始数据页 ----
    QPlainTextEdit *m_rawEdit = nullptr;
    QCheckBox *m_rawTimeChk = nullptr;
    QCheckBox *m_rawAutoScrollChk = nullptr;
    QStringList m_rawPending; // 待渲染的原始行(批量, 避免逐行渲染挤住事件循环)

    QTabWidget *m_tabs = nullptr; // 页签容器(按可见页决定刷新谁)

    // ---- 遥测数据页 ----
    QLabel *m_statRecords = nullptr;   // 样本总数
    QLabel *m_statPackets = nullptr;   // JSON 包总数
    QLabel *m_statAnomaly = nullptr;   // 异常计数(越界/解析失败)
    QLabel *m_statRate = nullptr;      // 样本/秒
    QLabel *m_statLineTotal = nullptr; // 串口行(JSON/日志)
    QLabel *m_statusPort = nullptr;

    quint64 m_anomalyCount = 0;        // 本次会话异常计数
    QString m_anomalyFile;             // 当日异常留证文件
    qint64  m_anomalyLogWindowMs = 0;  // 异常留证限流窗口
    int     m_anomalyLogCount = 0;     // 窗口内已写条数
    bool    m_anomalyLabelDirty = false; // 异常标签待刷新(批次更新, 防样式风暴)

    QTableWidget *m_latestTable = nullptr;
    QTableView   *m_historyView = nullptr;
    TelemetryTableModel *m_historyModel = nullptr;

    // ---- 字段选择面板 ----
    QTreeWidget *m_fieldTree = nullptr;
    QHash<QString, QTreeWidgetItem *> m_fieldItems; // key -> 叶子
    QHash<QString, QTreeWidgetItem *> m_groupItems; // 分组名 -> 顶层
    bool m_loadingFields = false;       // 程序批量设置勾选时防递归

    // ---- 遥测数据模型(自适应) ----
    QStringList m_fieldOrder;           // 字段注册顺序(已知 + 动态发现)
    QSet<QString> m_hidden;             // 被用户隐藏的字段
    QHash<QString, QJsonValue> m_lastValue; // key -> 最新值
    QList<TelemetryRecord> m_historySamples; // 记录表数据源(上限裁切)

    // ---- 状态/标志 LED 常显面板 ----
    QHash<QString, QLabel *> m_ledLamps; // 布尔字段 key -> LED 圆点 label
    QHash<QString, int> m_ledState;      // key -> 上次颜色状态(0灰/1绿/2红)

    // ---- 曲线页 ----
    QListWidget *m_plotFieldList = nullptr; // Y 字段(可多选)
    QComboBox   *m_plotXCombo = nullptr;    // X: 序号 / 设备时间 / 主机时间
    QComboBox   *m_plotModeCombo = nullptr; // 原始 / 一阶低通(可调τ)
    QSpinBox    *m_plotTauSpin = nullptr;   // 低通时间常数 τ(ms)
    QComboBox   *m_plotLayoutCombo = nullptr; // 同图叠加 / 每字段独立图
    QComboBox   *m_plotScaleCombo = nullptr;  // 纵轴共用 / 各曲线独立量程
    QSpinBox    *m_plotMaxSpin = nullptr;   // 每线最大点数
    // 显示范围过滤(仅曲线, 临时): key -> [最小, 最大], 范围外样本不画
    QCheckBox   *m_plotRangeChk = nullptr;
    QComboBox   *m_plotRangeFieldCombo = nullptr;
    QDoubleSpinBox *m_plotRangeMinSpin = nullptr;
    QDoubleSpinBox *m_plotRangeMaxSpin = nullptr;
    QHash<QString, QPair<double, double>> m_plotDisplayRange;
    QStackedWidget *m_plotArea = nullptr;   // 0=单图 1=分图(滚动)
    QScrollArea    *m_subplotScroll = nullptr;
    QWidget        *m_subplotHost = nullptr; // 分图模式承载页
    QChartView  *m_chartView = nullptr;     // 同图模式视图(唯一)
    // 曲线数据与显示解耦: 样本缓冲(保存每条样本的序号+ts+主机时间),
    // 切轴/改布局只重画, 不删数据
    QList<TelemetryRecord> m_curveSamples;  // 曲线源缓冲(上限=最大点数)
    QHash<QString, PlotSeriesGroup> m_plotGroups; // key -> 连续有效段集合
    QHash<QString, QColor> m_plotColorByKey;      // key -> 曲线颜色(增量段复用)
    QHash<QString, QChartView *> m_plotViews;      // key -> 分图视图(分图模式)
    QHash<QString, QValueAxis *> m_axisXByKey;     // key -> X 轴
    QHash<QString, QValueAxis *> m_axisYByKey;     // key -> Y 轴
    QDateTime    m_plotT0; // "主机时间(相对秒)"轴的基准时刻
    QHash<QString, PlotFilterState> m_lpState; // 一阶低通滤波状态(按key)
    QTimer      *m_plotAxisTimer = nullptr; // 合并坐标范围刷新, 防卡顿
    bool         m_plotDirty = false;       // 有新点待刷新坐标范围
    bool         m_plotZoomed = false;      // 用户框选放大中: 暂停自动量程

    // ---- 批量上屏机制(定时器驱动, 界面层与样本流解耦) ----
    QTimer *m_uiTimer = nullptr;            // 上屏定时器(50ms)
    QList<TelemetryRecord> m_pendingUi;     // 待上屏样本(批量处理)
    QSet<QString> m_latestChanged;          // 待刷新"最新值"的键(增量)
    QHash<QString, int> m_latestRowByKey;   // 字段key -> 最新值表行号

    // ---- 文件记录 ----
    QPushButton *m_recordBtn = nullptr;
    QPushButton *m_recordChooseBtn = nullptr;
    QPushButton *m_exportBtn = nullptr;        // 导出可读 JSON
    QPushButton *m_vboBtn = nullptr;           // 导出 VBO(.vbo)
    QPushButton *m_importBtn = nullptr;        // 导入历史记录回看
    QLabel      *m_recordPathLabel = nullptr;
    QLabel      *m_recordCountLabel = nullptr;
    QString      m_recordTarget;
    quint64      m_recordWritten = 0;

    static constexpr int kMaxHistoryRows = 500;  // 记录表最大样本数
    static constexpr int kMaxRawBlocks = 10000;  // 原始窗口最大行块数
};
