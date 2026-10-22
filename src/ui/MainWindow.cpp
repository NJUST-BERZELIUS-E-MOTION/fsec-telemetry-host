#include "ui/MainWindow.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QBrush>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRect>
#include <QScrollBar>
#include <QScrollArea>
#include <QSerialPortInfo>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableView>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <QVector>

#include <QtCharts/QAbstractAxis>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLegend>
#include <QtCharts/QLegendMarker>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include "core/JsonFrameParser.h"
#include "core/SerialReceiver.h"
#include "core/SinkManager.h"
#include "core/TelemetryRecord.h"
#include "sinks/FileSink.h"
#include "sinks/RawLogSink.h"
#include "sinks/TelemetrySink.h"
#include "ui/TelemetryTableModel.h"

// ---------------------------------------------------------------------
// 小工具: 向表格写入单元格(不存在则创建)
// ---------------------------------------------------------------------
static void setTableCell(QTableWidget *table, int row, int col,
                         const QString &text)
{
    QTableWidgetItem *item = table->item(row, col);
    if (!item) {
        item = new QTableWidgetItem;
        table->setItem(row, col, item);
    }
    item->setText(text);
}

// 解析记录文件里的 host_time(兼容本程序格式与 ISO)
static QDateTime parseHostTimeString(const QString &s)
{
    if (s.isEmpty())
        return QDateTime();
    QDateTime t = QDateTime::fromString(s, "yyyy-MM-dd HH:mm:ss.zzz");
    if (t.isValid())
        return t;
    t = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (t.isValid())
        return t;
    return QDateTime::fromString(s, Qt::ISODate);
}

// ---------------------------------------------------------------------
// 布尔(状态/标志)字段 -> LED:
//   activeIsBad=false(默认): 1=绿(在线/有效), 0=红(掉线/无效)
//   activeIsBad=true       : 1=红(冲突/不可靠等异常), 0=绿(正常)
static void applyBoolLed(QTableWidgetItem *item, const FieldSpec *spec,
                         const QJsonValue &v)
{
    if (!item || !spec || !spec->isBool)
        return;

    if (v.isUndefined() || v.isNull()) {
        item->setText("—");
        item->setForeground(QBrush(QColor(150, 150, 150)));
        item->setToolTip("无数据");
        return;
    }
    const bool on = v.isBool() ? v.toBool()
                               : (v.isDouble() && v.toDouble() != 0.0);
    item->setText("●");
    if (spec->activeIsBad) {
        // 置位即异常: 1=红(冲突/不可靠), 0=绿(正常)
        item->setForeground(QBrush(on ? QColor(210, 30, 30)
                                      : QColor(0, 150, 50)));
        item->setToolTip(on ? "异常: 冲突/不可靠(红)" : "正常(绿)");
    } else {
        item->setForeground(QBrush(on ? QColor(0, 150, 50)   // 绿=在线/有效
                                      : QColor(210, 30, 30))); // 红=掉线/无效
        item->setToolTip(on ? "在线 / 有效(绿)" : "掉线 / 无效(红)");
    }
}

// ---------------------------------------------------------------------
// 合理范围校验: 越界值标红并(曲线中)剔除; 布尔字段不做范围样式
// ---------------------------------------------------------------------
bool MainWindow::valueOutOfRange(const FieldSpec *spec, const QJsonValue &v)
{
    if (!spec || !spec->hasRange || !v.isDouble())
        return false;
    const double d = v.toDouble();
    return d < spec->minValue || d > spec->maxValue;
}

void MainWindow::applyRangeStyle(QTableWidgetItem *item, const FieldSpec *spec,
                                 const QJsonValue &v)
{
    if (!item || !spec || !spec->hasRange || !v.isDouble())
        return;
    if (!valueOutOfRange(spec, v))
        return;
    item->setForeground(QBrush(QColor(200, 0, 0)));
    const QString range = QString("[%1, %2]")
                              .arg(spec->minValue)
                              .arg(spec->maxValue);
    item->setToolTip(item->toolTip() + "\n越界: 合理范围 " + range);
}

// =====================================================================
// 构造 / 析构
// =====================================================================
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("LoRa 遥测上位机 - 串口 JSON 解析显示 (SerialJsonHost)");
    resize(1280, 800);
    setMinimumSize(1020, 640);

    // 字段显示配置: 优先 exe 旁 config/fields.json, 缺省用内置表
    const QString configPath =
        QCoreApplication::applicationDirPath() + "/config/fields.json";
    m_schema = QFile::exists(configPath) ? TelemetrySchema(configPath)
                                         : TelemetrySchema();

    // ---- 流水线对象(接收 -> 解析 -> 分发 -> 出口) ----
    m_receiver = new SerialReceiver(this);
    m_parser = new JsonFrameParser(this);
    m_sinks = new SinkManager;                 // 纯数据分发器, 不挂父对象
    m_rawSink = new RawLogSink(this);
    m_telemetrySink = new TelemetrySink(this);
    m_fileSink = new FileSink(this);

    // 注册数据处理出口(今后新增处理方式 = 实现 ISink 并在此注册)
    m_sinks->registerSink(m_rawSink);
    m_sinks->registerSink(m_telemetrySink);
    m_sinks->registerSink(m_fileSink);

    // ---- 界面骨架 ----
    QWidget *central = new QWidget(this);
    QVBoxLayout *root = new QVBoxLayout(central);
    root->setContentsMargins(6, 6, 6, 6);
    root->addWidget(buildControlBar());
    root->addWidget(buildRecordBar());

    QTabWidget *tabs = new QTabWidget(central);
    tabs->addTab(buildRawTab(), "原始数据");
    tabs->addTab(buildTelemetryTab(), "遥测数据");
    tabs->addTab(buildPlotTab(), "曲线");
    root->addWidget(tabs, 1);
    setCentralWidget(central);
    m_tabs = tabs;

    // 切页签时: 隐藏期间跳过的刷新在这里一次性补上(避免隐藏页拖慢前台)
    connect(tabs, &QTabWidget::currentChanged, this, [this](int idx) {
        if (idx == 1) {
            rebuildHistoryTable();
            refreshLatestValues();
            updateLedPanel();
        } else if (idx == 2) {
            rebuildPlotFromHistory();
        }
    });

    m_statusPort = new QLabel("未连接", this);
    statusBar()->addPermanentWidget(m_statusPort);
    statusBar()->showMessage("就绪");

    // ---- 信号连线 ----
    connect(m_receiver, &SerialReceiver::lineReceived,
            m_parser, &JsonFrameParser::onLineReceived);
    connect(m_parser, &JsonFrameParser::frameReady, this,
            [this](const Frame &f) { m_sinks->pushFrame(f); });
    connect(m_parser, &JsonFrameParser::statsChanged,
            this, &MainWindow::onParserStats);

    connect(m_rawSink, &RawLogSink::lineAppended,
            this, &MainWindow::appendRawLine);
    connect(m_telemetrySink, &TelemetrySink::recordReady,
            this, &MainWindow::onTelemetryRecord);
    connect(m_telemetrySink, &TelemetrySink::countersChanged,
            this, &MainWindow::onTelemetryCounters);
    connect(m_fileSink, &FileSink::recordChanged,
            this, &MainWindow::onRecordChanged);
    connect(m_fileSink, &FileSink::recordError,
            this, &MainWindow::onRecordError);

    connect(m_receiver, &SerialReceiver::opened,
            this, &MainWindow::onConnectionOpened);
    connect(m_receiver, &SerialReceiver::closed,
            this, &MainWindow::onConnectionClosed);
    connect(m_receiver, &SerialReceiver::errorOccurred,
            this, &MainWindow::onPortError);

    // 端口初始枚举 + 恢复上次选择的端口/波特率
    refreshPorts();
    QSettings settings;
    const QString lastPort = settings.value("lastPort").toString();
    const int lastBaud = settings.value("lastBaud", 115200).toInt();
    if (!lastPort.isEmpty()) {
        const int idx = m_portCombo->findData(lastPort);
        if (idx >= 0)
            m_portCombo->setCurrentIndex(idx);
    }
    const int baudIdx = m_baudCombo->findText(QString::number(lastBaud));
    if (baudIdx >= 0)
        m_baudCombo->setCurrentIndex(baudIdx);
    else
        m_baudCombo->setCurrentText(QString::number(lastBaud));

    // 上屏定时器: 所有界面刷新(最新值/记录/曲线)由它批量驱动
    m_uiTimer = new QTimer(this);
    m_uiTimer->setInterval(20); // 20ms 批量上屏: 兼顾流畅与低延迟(原50ms体感偏慢)
    connect(m_uiTimer, &QTimer::timeout, this, &MainWindow::onUiTick);
    m_uiTimer->start();

    if (!QFile::exists(configPath))
        appendSystemLine("未找到 config/fields.json, 使用内置字段表显示");
}

MainWindow::~MainWindow()
{
    delete m_sinks; // SinkManager 由本类独占
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_receiver)
        m_receiver->closePort();
    event->accept();
}

// =====================================================================
// 视图构建
// =====================================================================
QWidget *MainWindow::buildControlBar()
{
    QWidget *bar = new QWidget(this);
    QHBoxLayout *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);

    h->addWidget(new QLabel("串口:", bar));
    m_portCombo = new QComboBox(bar);
    m_portCombo->setMinimumWidth(190);
    m_portCombo->setToolTip("当前可用的串口(点击\"刷新\"重新枚举)");
    h->addWidget(m_portCombo);

    QPushButton *refreshBtn = new QPushButton("刷新", bar);
    connect(refreshBtn, &QPushButton::clicked,
            this, &MainWindow::refreshPorts);
    h->addWidget(refreshBtn);

    h->addSpacing(12);
    h->addWidget(new QLabel("波特率:", bar));
    m_baudCombo = new QComboBox(bar);
    m_baudCombo->setEditable(true);
    m_baudCombo->addItems({ "9600", "19200", "38400", "57600",
                            "115200", "230400", "460800", "921600" });
    m_baudCombo->setCurrentText("115200");
    m_baudCombo->lineEdit()->setValidator(
        new QIntValidator(300, 4000000, m_baudCombo));
    m_baudCombo->setMinimumWidth(100);
    m_baudCombo->setToolTip("默认 115200, 与固件 uart_init(115200) 一致; 8 数据位/无校验/1 停止位");
    h->addWidget(m_baudCombo);

    h->addSpacing(12);
    m_openBtn = new QPushButton("打开串口", bar);
    m_openBtn->setCheckable(true);
    m_openBtn->setMinimumWidth(96);
    connect(m_openBtn, &QPushButton::toggled,
            this, &MainWindow::onOpenToggled);
    h->addWidget(m_openBtn);

    h->addSpacing(12);
    m_pauseChk = new QCheckBox("暂停刷新", bar);
    m_pauseChk->setToolTip("暂停界面呈现(接收照常进行, 计数照常累加)");
    connect(m_pauseChk, &QCheckBox::toggled,
            this, &MainWindow::onPauseToggled);
    h->addWidget(m_pauseChk);

    h->addStretch(1);
    return bar;
}

QWidget *MainWindow::buildRecordBar()
{
    QWidget *bar = new QWidget(this);
    QHBoxLayout *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);

    h->addWidget(new QLabel("记录:", bar));

    m_recordPathLabel = new QLabel(bar);
    m_recordPathLabel->setMinimumWidth(320);
    m_recordPathLabel->setToolTip("一次记录 = 一个文件夹(内含 burst/data/event/other.jsonl 与 meta.json)");
    h->addWidget(m_recordPathLabel, 1);

    m_recordChooseBtn = new QPushButton("记录文件夹…", bar);
    m_recordChooseBtn->setToolTip(
        "选择本条记录的文件夹名(会自动创建该文件夹):\n"
        "  <文件夹>/burst.jsonl / data.jsonl / event.jsonl / other.jsonl\n"
        "停止记录时再写 <文件夹>/meta.json(会话信息+字段定义+计数)");
    connect(m_recordChooseBtn, &QPushButton::clicked,
            this, &MainWindow::chooseRecordFile);
    h->addWidget(m_recordChooseBtn);

    m_exportBtn = new QPushButton("导出可读JSON", bar);
    m_exportBtn->setToolTip(
        "把本次会话的 JSONL 转成缩进 2 空格的 <基名>_<类型>.pretty.json, 便于阅读/归档");
    m_exportBtn->setEnabled(false);
    connect(m_exportBtn, &QPushButton::clicked,
            this, &MainWindow::exportReadableJson);
    h->addWidget(m_exportBtn);

    m_vboBtn = new QPushButton("导出VBO…", bar);
    m_vboBtn->setToolTip(
        "把本次会话的记录导出为 VBOX 文本格式(.vbo):\n"
        "[header]/[channel units]/[column names]/[data]\n"
        "时间基为设备 ts(相对首样本秒), 缺 ts 用主机时间; 缺通道值填 0");
    m_vboBtn->setEnabled(false);
    connect(m_vboBtn, &QPushButton::clicked,
            this, &MainWindow::exportVbo);
    h->addWidget(m_vboBtn);

    m_importBtn = new QPushButton("导入查看…", bar);
    m_importBtn->setToolTip(
        "导入之前保存的记录(可多选):\n"
        "记录文件夹里的 burst/data/event/other.jsonl、*.pretty.json、meta.json\n"
        "导入只用于查看, 不会写入记录文件");
    connect(m_importBtn, &QPushButton::clicked,
            this, &MainWindow::importRecords);
    h->addWidget(m_importBtn);

    m_recordBtn = new QPushButton("开始记录", bar);
    m_recordBtn->setCheckable(true);
    m_recordBtn->setMinimumWidth(96);
    m_recordBtn->setToolTip("手动开始/停止把收到的遥测按'一行一条样本'写入记录文件");
    connect(m_recordBtn, &QPushButton::toggled,
            this, &MainWindow::onRecordToggled);
    h->addWidget(m_recordBtn);

    m_recordCountLabel = new QLabel("0 条", bar);
    h->addWidget(m_recordCountLabel);
    h->addStretch(1);

    updateRecordControls();
    return bar;
}

QWidget *MainWindow::buildRawTab()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *vl = new QVBoxLayout(page);
    vl->setContentsMargins(4, 4, 4, 4);

    QHBoxLayout *tool = new QHBoxLayout;
    m_rawTimeChk = new QCheckBox("显示时间戳", page);
    m_rawTimeChk->setChecked(true);
    m_rawAutoScrollChk = new QCheckBox("自动滚动", page);
    m_rawAutoScrollChk->setChecked(true);
    QPushButton *clearBtn = new QPushButton("清空", page);
    connect(clearBtn, &QPushButton::clicked,
            this, &MainWindow::clearRawView);
    tool->addWidget(m_rawTimeChk);
    tool->addWidget(m_rawAutoScrollChk);
    tool->addStretch(1);
    tool->addWidget(clearBtn);
    vl->addLayout(tool);

    m_rawEdit = new QPlainTextEdit(page);
    m_rawEdit->setReadOnly(true);
    m_rawEdit->setMaximumBlockCount(kMaxRawBlocks);
    m_rawEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_rawEdit->setPlaceholderText(
        "打开串口后, 收到的每一行会显示在这里:\n"
        "  - 绿色 = 解析成功的 JSON 行(遥测包)\n"
        "  - 白色 = 设备日志/其它文本行\n"
        "  - 蓝色 = 本程序系统消息");
    vl->addWidget(m_rawEdit, 1);
    return page;
}

QWidget *MainWindow::buildTelemetryTab()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *vl = new QVBoxLayout(page);
    vl->setContentsMargins(4, 4, 4, 4);

    // 统计栏
    QHBoxLayout *stats = new QHBoxLayout;
    stats->addWidget(new QLabel("样本总数:", page));
    m_statRecords = new QLabel("0", page);
    m_statRecords->setStyleSheet("font-weight:bold;");
    stats->addWidget(m_statRecords);
    stats->addSpacing(14);
    stats->addWidget(new QLabel("JSON包总数:", page));
    m_statPackets = new QLabel("0", page);
    stats->addWidget(m_statPackets);
    stats->addSpacing(14);
    stats->addWidget(new QLabel("速率:", page));
    m_statRate = new QLabel("0 样本/s", page);
    stats->addWidget(m_statRate);
    stats->addSpacing(14);
    stats->addWidget(new QLabel("串口行:", page));
    m_statLineTotal = new QLabel("0 (JSON 0 / 日志 0)", page);
    stats->addWidget(m_statLineTotal);
    stats->addSpacing(14);
    stats->addWidget(new QLabel("异常:", page));
    m_statAnomaly = new QLabel("0", page);
    m_statAnomaly->setToolTip(
        "越界值/解析失败行计数; 原始行自动留证到 recordings/anomalies/anomaly_日期.jsonl");
    stats->addWidget(m_statAnomaly);
    stats->addStretch(1);
    QPushButton *clearBtn = new QPushButton("清空遥测", page);
    connect(clearBtn, &QPushButton::clicked,
            this, &MainWindow::clearTelemetryView);
    stats->addWidget(clearBtn);
    vl->addLayout(stats);

    // 左右分栏: 左=字段选择面板, 右=最新值+记录
    QSplitter *hsplit = new QSplitter(Qt::Horizontal, page);
    hsplit->addWidget(buildFieldPanel());
    hsplit->setStretchFactor(0, 0);

    QWidget *right = new QWidget(hsplit);
    QVBoxLayout *rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);

    QSplitter *vsplit = new QSplitter(Qt::Vertical, right);

    QGroupBox *latestBox = new QGroupBox("最新值(勾选字段)", vsplit);
    QVBoxLayout *ll = new QVBoxLayout(latestBox);
    m_latestTable = new QTableWidget(latestBox);
    m_latestTable->setColumnCount(2);
    m_latestTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_latestTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_latestTable->verticalHeader()->setVisible(false);
    m_latestTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_latestTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ll->addWidget(m_latestTable);
    vsplit->addWidget(latestBox);

    QGroupBox *historyBox = new QGroupBox(
        "接收记录(最近 " + QString::number(kMaxHistoryRows) +
            " 条样本, 按接收内容自适应列)", vsplit);
    QVBoxLayout *hl = new QVBoxLayout(historyBox);
    // 用 QTableView + 自定义模型取代 QTableWidget:
    // 数据只在内存模型里, 视图仅绘制可见行, 实时插入/滚动开销大幅降低
    m_historyModel = new TelemetryTableModel(this);
    m_historyModel->setSchema(&m_schema);
    m_historyModel->setMaxRows(kMaxHistoryRows);
    m_historyView = new QTableView(historyBox);
    m_historyView->setModel(m_historyModel);
    m_historyView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_historyView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_historyView->setWordWrap(false);
    m_historyView->verticalHeader()->setVisible(false);
    m_historyView->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Interactive);
    m_historyView->horizontalHeader()->setStretchLastSection(false);
    m_historyView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    hl->addWidget(m_historyView);
    vsplit->addWidget(historyBox);

    vsplit->setStretchFactor(0, 2);
    vsplit->setStretchFactor(1, 3);
    rv->addWidget(vsplit, 1);

    hsplit->addWidget(right);
    hsplit->setStretchFactor(1, 1);

    // 右侧: 状态/标志 LED 常显面板(始终显示)
    hsplit->addWidget(buildLedPanel());
    hsplit->setStretchFactor(2, 0);
    hsplit->setSizes({ 210, 720, 205 });
    vl->addWidget(hsplit, 1);

    // 表格/面板初始化
    refreshLatestValues();
    rebuildHistoryTable();
    updateLedPanel();
    return page;
}

QWidget *MainWindow::buildFieldPanel()
{
    QWidget *panel = new QWidget(this);
    QVBoxLayout *pl = new QVBoxLayout(panel);
    pl->setContentsMargins(0, 0, 6, 0);

    QHBoxLayout *head = new QHBoxLayout;
    QLabel *title = new QLabel("显示字段(勾选=显示)", panel);
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    head->addWidget(title);
    head->addStretch(1);
    QPushButton *allBtn = new QPushButton("全选", panel);
    QPushButton *noneBtn = new QPushButton("全不选", panel);
    connect(allBtn, &QPushButton::clicked, this, &MainWindow::selectAllFields);
    connect(noneBtn, &QPushButton::clicked, this, &MainWindow::selectNoFields);
    head->addWidget(allBtn);
    head->addWidget(noneBtn);
    pl->addLayout(head);

    m_fieldTree = new QTreeWidget(panel);
    m_fieldTree->setHeaderHidden(true);
    m_fieldTree->setRootIsDecorated(true);
    m_fieldTree->setToolTip("勾选要显示的字段; 双击分组名可整组开关");
    connect(m_fieldTree, &QTreeWidget::itemChanged,
            this, &MainWindow::onFieldItemChanged);
    connect(m_fieldTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!item || item->childCount() == 0)
                    return;
                const QString key = item->data(0, Qt::UserRole).toString();
                if (!key.isEmpty())
                    return; // 双击叶子不做特殊处理
                const bool newState =
                    (item->child(0)->checkState(0) != Qt::Checked);
                m_loadingFields = true;
                for (int i = 0; i < item->childCount(); ++i)
                    item->child(i)->setCheckState(
                        0, newState ? Qt::Checked : Qt::Unchecked);
                m_loadingFields = false;
                // 用整棵树状态刷新隐藏集合并重画
                m_hidden.clear();
                for (auto it = m_fieldItems.constBegin();
                     it != m_fieldItems.constEnd(); ++it) {
                    if (it.value()->checkState(0) != Qt::Checked)
                        m_hidden.insert(it.key());
                }
                saveHiddenFields();
                refreshLatestValues();
                rebuildHistoryTable();
            });
    pl->addWidget(m_fieldTree, 1);

    // 注册全部已知字段(顺序即显示顺序), 再恢复上次隐藏选择
    m_loadingFields = true;
    for (const FieldSpec &sp : m_schema.fields())
        registerField(sp.key, &sp);
    m_loadingFields = false;
    loadHiddenFields();

    return panel;
}

// =====================================================================
// 字段注册 / 可见性
// =====================================================================
void MainWindow::registerField(const QString &key, const FieldSpec *spec)
{
    if (m_fieldOrder.contains(key))
        return;

    m_fieldOrder.append(key);

    if (!m_fieldTree)
        return;

    const QString group = spec ? spec->group : "动态字段";
    QTreeWidgetItem *groupItem = m_groupItems.value(group, nullptr);
    if (!groupItem) {
        groupItem = new QTreeWidgetItem(m_fieldTree, { group });
        QFont gf = groupItem->font(0);
        gf.setBold(true);
        groupItem->setFont(0, gf);
        m_groupItems.insert(group, groupItem);
    }

    auto *leaf = new QTreeWidgetItem(groupItem);
    QString text = spec ? spec->label : key;
    if (spec && !spec->unit.isEmpty())
        text += " [" + spec->unit + "]";
    leaf->setText(0, text);
    leaf->setToolTip(0, "JSON键: " + key +
                         (spec && spec->label != key
                              ? "  (" + spec->label + ")"
                              : QString()));
    leaf->setData(0, Qt::UserRole, key);
    leaf->setCheckState(0, Qt::Checked);
    m_fieldItems.insert(key, leaf);

    groupItem->setExpanded(true);
}

void MainWindow::loadHiddenFields()
{
    if (!m_fieldTree)
        return;
    QSettings settings;
    const QStringList hidden = settings.value("hiddenFields").toStringList();
    m_hidden = QSet<QString>(hidden.begin(), hidden.end());

    m_loadingFields = true;
    for (auto it = m_fieldItems.constBegin(); it != m_fieldItems.constEnd();
         ++it) {
        it.value()->setCheckState(0,
                                  m_hidden.contains(it.key()) ? Qt::Unchecked
                                                              : Qt::Checked);
    }
    m_loadingFields = false;
}

void MainWindow::saveHiddenFields() const
{
    QStringList list;
    for (const QString &key : m_hidden)
        list.append(key);
    list.sort();
    QSettings settings;
    settings.setValue("hiddenFields", list);
}

QStringList MainWindow::orderedVisibleKeys() const
{
    QStringList keys;
    for (const QString &key : m_fieldOrder) {
        if (!m_hidden.contains(key))
            keys.append(key);
    }
    return keys;
}

void MainWindow::onFieldItemChanged(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (m_loadingFields || !item)
        return;

    const QString key = item->data(0, Qt::UserRole).toString();
    if (key.isEmpty())
        return; // 分组节点由双击逻辑处理

    if (item->checkState(0) == Qt::Checked)
        m_hidden.remove(key);
    else
        m_hidden.insert(key);
    saveHiddenFields();

    refreshLatestValues();
    rebuildHistoryTable();
}

void MainWindow::selectAllFields()
{
    m_hidden.clear();
    if (m_fieldTree) {
        m_loadingFields = true;
        for (auto it = m_fieldItems.constBegin(); it != m_fieldItems.constEnd();
             ++it)
            it.value()->setCheckState(0, Qt::Checked);
        m_loadingFields = false;
    }
    saveHiddenFields();
    refreshLatestValues();
    rebuildHistoryTable();
}

void MainWindow::selectNoFields()
{
    m_hidden.clear();
    if (m_fieldTree) {
        m_loadingFields = true;
        for (auto it = m_fieldItems.constBegin(); it != m_fieldItems.constEnd();
             ++it) {
            it.value()->setCheckState(0, Qt::Unchecked);
            m_hidden.insert(it.key());
        }
        m_loadingFields = false;
    }
    saveHiddenFields();
    refreshLatestValues();
    rebuildHistoryTable();
}

// =====================================================================
// 自适应遥测显示(样本驱动)
// =====================================================================
void MainWindow::onTelemetryRecord(const TelemetryRecord &record)
{
    // 1) 自动注册未知字段(内容自适应)
    bool discoveredNew = false;
    for (auto it = record.fields.constBegin(); it != record.fields.constEnd();
         ++it) {
        const QString key = it.key();
        if (!m_schema.find(key) && isRecordMetaKey(key))
            continue; // 未配置的元数据键(seq/host_time 等)不作为字段
        if (!m_fieldOrder.contains(key)) {
            registerField(key, m_schema.find(key));
            discoveredNew = true;
        }
    }

    // 2) 只更新内存缓存(不碰表格), 界面由 20ms 定时器批量上屏
    for (auto it = record.fields.constBegin(); it != record.fields.constEnd();
         ++it) {
        m_lastValue.insert(it.key(), it.value());
        m_latestChanged.insert(it.key());
    }
    // LED 与表格一起放到定时器里刷新, 避免每条样本都重设样式

    // 2b) 异常检测: 越界字段计数 + 原始行留证(每样本最多记一条)
    QStringList badFields;
    for (auto it = record.fields.constBegin(); it != record.fields.constEnd();
         ++it) {
        const FieldSpec *sp = m_schema.find(it.key());
        if (valueOutOfRange(sp, it.value())) {
            badFields << QString("%1=%2(合理范围 %3~%4)")
                             .arg(it.key())
                             .arg(it.value().toDouble())
                             .arg(sp->minValue)
                             .arg(sp->maxValue);
        }
    }
    if (!badFields.isEmpty()) {
        ++m_anomalyCount;
        m_anomalyLabelDirty = true;
        logAnomaly("range", record.rawLine,
                   "越界: " + badFields.join("; "));
    }

    // 3) 入历史缓存 + 待上屏队列
    m_historySamples.append(record);
    while (m_historySamples.size() > kMaxHistoryRows)
        m_historySamples.removeFirst();
    m_pendingUi.append(record);
    if (m_pendingUi.size() > 2000)
        m_pendingUi.removeFirst(); // 极端洪峰保护(定时器会很快消化)

    if (discoveredNew) {
        // 新列出现: 罕见, 立即整表重建(重建会清空待上屏, 避免重复)
        rebuildHistoryTable();
        rebuildPlotFromHistory();
        refreshLatestValues(); // 顺带重建 key->行号映射
    }
}

void MainWindow::refreshLatestValues()
{
    if (!m_latestTable)
        return;

    const QStringList keys = orderedVisibleKeys();
    m_latestTable->clear();
    m_latestTable->setRowCount(0);
    m_latestRowByKey.clear();
    m_latestTable->setHorizontalHeaderLabels({ "字段", "最新值" });

    int row = 0;
    for (const QString &key : keys) {
        m_latestTable->insertRow(row);
        m_latestRowByKey.insert(key, row); // 增量刷新用的行号映射
        const FieldSpec *sp = m_schema.find(key);
        QString label;
        if (sp)
            label = (sp->unit.isEmpty()) ? sp->label + " (" + key + ")"
                                         : sp->label + " [" + sp->unit + "]";
        else
            label = key + " (未知)";
        setTableCell(m_latestTable, row, 0, label);

        const QJsonValue v = m_lastValue.value(key);
        setTableCell(m_latestTable, row, 1,
                     v.isUndefined() ? "—" : formatValue(sp, v));
        QTableWidgetItem *valueItem = m_latestTable->item(row, 1);
        if (sp && sp->isBool) {
            applyBoolLed(valueItem, sp, v);
        } else {
            valueItem->setToolTip("JSON键: " + key);
            if (sp)
                applyRangeStyle(valueItem, sp, v);
        }
        ++row;
    }
}

void MainWindow::rebuildHistoryTable()
{
    if (!m_historyModel || !m_historyView)
        return;

    m_historyModel->resetAll(orderedVisibleKeys(), m_historySamples);
    m_historyView->resizeColumnsToContents(); // 仅在重建时做一次列宽适配
    m_historyView->scrollToBottom();

    // 把尚未上屏的样本先补进曲线缓冲, 再清空队列(避免曲线丢最后一批点)
    flushPendingToCurveBuffer();
    m_latestChanged.clear();
}

// =====================================================================
// 串口控制
// =====================================================================
void MainWindow::refreshPorts()
{
    const QString prev = m_portCombo->currentData().toString();
    m_portCombo->clear();

    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        QString desc = info.description().trimmed();
        if (desc.isEmpty())
            desc = info.manufacturer().trimmed();
        const QString text =
            desc.isEmpty() ? info.portName()
                           : info.portName() + " (" + desc + ")";
        m_portCombo->addItem(text, info.portName());
    }

    if (!prev.isEmpty()) {
        const int idx = m_portCombo->findData(prev);
        if (idx >= 0)
            m_portCombo->setCurrentIndex(idx);
    }
}

void MainWindow::onOpenToggled(bool checked)
{
    if (checked) {
        const QString port = m_portCombo->currentData().toString();
        const qint32 baud = m_baudCombo->currentText().toInt();

        if (port.isEmpty()) {
            appendSystemLine("未选择串口, 请先点击\"刷新\"");
            QSignalBlocker block(m_openBtn);
            m_openBtn->setChecked(false);
            return;
        }
        if (baud <= 0) {
            appendSystemLine("波特率无效");
            QSignalBlocker block(m_openBtn);
            m_openBtn->setChecked(false);
            return;
        }

        QSettings settings;
        settings.setValue("lastPort", port);
        settings.setValue("lastBaud", baud);

        const bool ok = m_receiver->openPort(port, baud);
        if (!ok) {
            QSignalBlocker block(m_openBtn);
            m_openBtn->setChecked(false);
        }
    } else {
        m_receiver->closePort();
    }
}

void MainWindow::onPortError(const QString &message)
{
    appendRawHtml("<span style='color:#cc0000;'>[错误] " +
                  message.toHtmlEscaped() + "</span>");
    statusBar()->showMessage(message, 5000);
}

void MainWindow::onConnectionOpened()
{
    const QString port = m_receiver->portName();
    const QString baud = m_baudCombo->currentText();

    // 新一轮会话: 解析器计数、出口状态、显示区全部复位
    m_parser->reset();
    m_sinks->resetAll();
    m_rawEdit->clear();
    clearTelemetryView();

    // 异常计数/留证文件也按"本会话"重新开始
    m_anomalyCount = 0;
    m_anomalyFile.clear();
    updateAnomalyLabel();

    m_portCombo->setEnabled(false);
    m_baudCombo->setEnabled(false);
    m_openBtn->setText("关闭串口");
    m_openBtn->setChecked(true);
    m_statusPort->setText("已连接: " + port + " @ " + baud + " 8N1");
    statusBar()->showMessage("串口已打开");
    appendSystemLine("串口 " + port + " @ " + baud +
                     " 已打开(8 数据位 / 无校验 / 1 停止位)");
}

void MainWindow::onConnectionClosed()
{
    m_portCombo->setEnabled(true);
    m_baudCombo->setEnabled(true);
    m_openBtn->setText("打开串口");
    m_openBtn->setChecked(false);
    m_statusPort->setText("未连接");
    statusBar()->showMessage("串口已关闭");
    appendSystemLine("串口已关闭");
}

// =====================================================================
// 数据显示 / 统计
// =====================================================================
// 原始页也批量渲染: 单次上屏最多 300 行, 防止逐行 appendHtml 挤住事件循环
void MainWindow::flushRawPending()
{
    if (!m_rawEdit || m_rawPending.isEmpty())
        return;

    int n = 0;
    while (!m_rawPending.isEmpty() && n < 300) {
        m_rawEdit->appendHtml(m_rawPending.takeFirst());
        ++n;
    }
    if (m_rawAutoScrollChk && m_rawAutoScrollChk->isChecked())
        m_rawEdit->verticalScrollBar()->setValue(
            m_rawEdit->verticalScrollBar()->maximum());
}

void MainWindow::appendRawHtml(const QString &html)
{
    flushRawPending(); // 保证系统消息顺序在已缓存行之后
    m_rawEdit->appendHtml(html);
    if (m_rawAutoScrollChk && m_rawAutoScrollChk->isChecked())
        m_rawEdit->verticalScrollBar()->setValue(
            m_rawEdit->verticalScrollBar()->maximum());
}

void MainWindow::appendSystemLine(const QString &text)
{
    appendRawHtml("<span style='color:#1550b0;'>[系统] " +
                  text.toHtmlEscaped() + "</span>");
}

void MainWindow::appendRawLine(quint64 seq, const QDateTime &time, bool isJson,
                               const QString &text)
{
    QString prefix;
    if (m_rawTimeChk && m_rawTimeChk->isChecked())
        prefix = "<span style='color:#808080;'>" +
                 time.toString("HH:mm:ss.zzz") + " </span>";

    // 形态像 JSON 但解析失败: 计入异常并留证(排查串口/LoRa 位错)
    if (!isJson) {
        const QString t = text.trimmed();
        if (t.startsWith('{') || t.startsWith('[')) {
            ++m_anomalyCount;
            m_anomalyLabelDirty = true;
            logAnomaly("unparsed", text.toUtf8(), "JSON 解析失败(原始行已留证)");
        }
    }

    // JSON 绿色, 设备日志白(保持现有配色)
    const QString color = isJson ? "#0a7d32" : "#ffffff";
    m_rawPending.append(prefix + "<span style='color:" + color + ";'>" +
                        text.toHtmlEscaped() + "</span>");
    if (m_rawPending.size() > 4000)
        m_rawPending.removeFirst(); // 极端洪峰: 丢最旧未渲染行, 保证不卡死
}

// 异常留证: 写到 recordings/anomalies/anomaly_yyyyMMdd.jsonl
// 限流: 每秒最多 20 条, 避免磁盘 I/O 把界面事件循环拖住
void MainWindow::logAnomaly(const QString &reason, const QByteArray &raw,
                            const QString &detail)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_anomalyLogWindowMs > 1000) {
        m_anomalyLogWindowMs = now;
        m_anomalyLogCount = 0;
    }
    if (m_anomalyLogCount >= 20)
        return; // 超出限流: 只计数, 不落盘
    ++m_anomalyLogCount;

    if (m_anomalyFile.isEmpty()) {
        const QString dir = defaultRecordDir() + "/anomalies";
        QDir().mkpath(dir);
        m_anomalyFile = dir + "/anomaly_" +
                        QDate::currentDate().toString("yyyyMMdd") + ".jsonl";
    }
    QFile f(m_anomalyFile);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    QJsonObject o;
    o.insert("host_time",
             QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"));
    o.insert("reason", reason);
    o.insert("detail", detail);
    o.insert("raw", QString::fromUtf8(raw));
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.write("\n");
    f.close();
}

void MainWindow::updateAnomalyLabel()
{
    if (!m_statAnomaly)
        return;
    m_statAnomaly->setText(QString::number(m_anomalyCount));
    m_statAnomaly->setStyleSheet(
        m_anomalyCount > 0 ? "color:#c00000;font-weight:bold;" : QString());
}

void MainWindow::onParserStats(quint64 lines, quint64 jsonLines,
                               quint64 logLines)
{
    if (m_statLineTotal) {
        m_statLineTotal->setText(
            QString("%1 (JSON %2 / 日志 %3)")
                .arg(lines).arg(jsonLines).arg(logLines));
    }
}

void MainWindow::onTelemetryCounters(quint64 records, quint64 packets,
                                     int recordsPerSec)
{
    if (m_statRecords)
        m_statRecords->setText(QString::number(records));
    if (m_statPackets)
        m_statPackets->setText(QString::number(packets));
    if (m_statRate)
        m_statRate->setText(QString("%1 样本/s").arg(recordsPerSec));
}

// =====================================================================
// 视图操作
// =====================================================================
void MainWindow::clearRawView()
{
    m_rawPending.clear();
    m_rawEdit->clear();
    appendSystemLine("原始数据已清空");
}

void MainWindow::clearTelemetryView()
{
    m_historySamples.clear();
    m_lastValue.clear();
    m_pendingUi.clear();
    m_latestChanged.clear();
    m_curveSamples.clear(); // 曲线源缓冲同步清空
    refreshLatestValues();
    rebuildHistoryTable();
    updateLedPanel();      // LED 全部回到"无数据"灰
    rebuildPlotFromHistory(); // 曲线清空并重建空系列
    // 清空后样本序号/统计从 0 重新开始(与表格一致)
    if (m_telemetrySink)
        m_telemetrySink->onReset();
}

void MainWindow::onPauseToggled(bool checked)
{
    m_sinks->setAllPaused(checked);
    appendSystemLine(checked ? "界面刷新已暂停(接收仍在进行)"
                             : "界面刷新已恢复");
}

// =====================================================================
// 字段表头 / 格式化
// =====================================================================
QString MainWindow::headerForField(const FieldSpec &f) const
{
    QString text = f.label;
    if (f.label != f.key)
        text += " (" + f.key + ")";
    if (!f.unit.isEmpty())
        text += " [" + f.unit + "]";
    return text;
}

QString MainWindow::formatValue(const FieldSpec *spec, const QJsonValue &v)
{
    if (v.isDouble()) {
        if (spec && spec->isBool)
            return v.toDouble() != 0.0 ? "开" : "关";
        const int dec = spec ? spec->decimals : 3;
        return QString::number(v.toDouble(), 'f', dec);
    }
    if (v.isString())
        return v.toString();
    if (v.isBool())
        return v.toBool() ? "开" : "关";
    if (v.isNull())
        return "-";
    return v.toString(); // 数组/对象等罕见情况
}

// =====================================================================
// 文件记录(结构化样本 JSONL)
// =====================================================================
QString MainWindow::defaultRecordDir() const
{
    return QCoreApplication::applicationDirPath() + "/recordings";
}

QString MainWindow::suggestedRecordPath() const
{
    QSettings settings;
    const QString dir =
        settings.value("recordDir", defaultRecordDir()).toString();
    const QString name = "SerialJsonHost_" +
                         QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    return dir + "/" + name;
}

void MainWindow::chooseRecordFile()
{
    const QFileInfo cur(suggestedRecordPath());
    // 在这里输入"文件夹名"(也可先选目录再输入名字); 程序会创建该文件夹
    const QString path = QFileDialog::getSaveFileName(
        this, "选择记录文件夹名(将创建同名文件夹)",
        cur.absolutePath() + "/" + cur.fileName(),
        "所有文件 (*.*)");
    if (path.isEmpty())
        return;

    m_recordTarget = path;
    QSettings settings;
    settings.setValue("recordDir", QFileInfo(path).absolutePath());
    updateRecordControls();
    appendSystemLine("记录文件夹已设置: " + path +
                     "  (将生成 burst/data/event/other.jsonl 与 meta.json)");
}

void MainWindow::onRecordToggled(bool checked)
{
    if (checked) {
        QString path = m_recordTarget;
        if (path.isEmpty()) {
            path = suggestedRecordPath();
            m_recordTarget = path;
            appendSystemLine("未手动指定保存位置, 使用默认记录文件夹: " + path);
        }

        // 把会话信息与字段定义交给记录出口(用于定序输出与 meta 文件)
        m_fileSink->setSessionInfo(m_receiver->portName(),
                                   m_baudCombo->currentText().toInt());
        QStringList order;
        for (const FieldSpec &sp : m_schema.fields())
            order << sp.key;
        m_fileSink->setFieldOrder(order);
        m_fileSink->setFieldDefs(buildFieldDefsJson());

        if (m_fileSink->start(path)) {
            updateRecordControls();
            appendSystemLine("开始记录 → 文件夹 " + m_fileSink->basePath() +
                             "  (burst/data/event/other.jsonl)");
        } else {
            QSignalBlocker block(m_recordBtn);
            m_recordBtn->setChecked(false);
            updateRecordControls();
        }
    } else {
        const QString base = m_fileSink->basePath();
        m_fileSink->stop();
        updateRecordControls();
        appendSystemLine("记录已停止, 文件夹: " + base +
                         "  (已写 meta.json)");
    }
}

void MainWindow::onRecordChanged(bool active, quint64 written)
{
    m_recordWritten = written;
    Q_UNUSED(active);
    updateRecordControls();
}

void MainWindow::onRecordError(const QString &message)
{
    appendRawHtml("<span style='color:#cc0000;'>[记录] " +
                  message.toHtmlEscaped() + "</span>");
    if (m_fileSink && !m_fileSink->isRecording() && m_recordBtn) {
        QSignalBlocker block(m_recordBtn);
        m_recordBtn->setChecked(false);
    }
    updateRecordControls();
}

void MainWindow::updateRecordControls()
{
    if (!m_recordBtn || !m_recordPathLabel || !m_recordCountLabel)
        return;

    const bool active = m_fileSink && m_fileSink->isRecording();
    m_recordBtn->setText(active ? "停止记录" : "开始记录");
    m_recordChooseBtn->setEnabled(!active);
    if (m_exportBtn)
        m_exportBtn->setEnabled(!active && m_fileSink &&
                                m_fileSink->hasSessionFiles());
    if (m_vboBtn)
        m_vboBtn->setEnabled((m_fileSink && m_fileSink->hasSessionFiles()) ||
                             !m_curveSamples.isEmpty() ||
                             !m_historySamples.isEmpty());

    const QString path = m_recordTarget.isEmpty() ? suggestedRecordPath()
                                                  : m_recordTarget;
    const QString base = active ? m_fileSink->basePath() : path;
    QFontMetrics fm(m_recordPathLabel->font());
    m_recordPathLabel->setText(
        fm.elidedText(base + "  →  burst/data/event/other.jsonl + meta.json",
                      Qt::ElideMiddle, 520));
    m_recordPathLabel->setToolTip(
        base + "\n一次记录 = 一个文件夹:\n"
               "  burst.jsonl / data.jsonl / event.jsonl / other.jsonl\n"
               "  停止时另写 meta.json(会话信息/字段定义/计数)" +
        (m_recordTarget.isEmpty()
             ? QString("\n(未手动指定; 点\"记录文件夹…\"可自定义名字与位置)")
             : QString()));

    m_recordCountLabel->setText(QString("%1 条").arg(m_recordWritten));
}

// 字段定义(写入记录 meta, 便于日后分析/复现)
QJsonArray MainWindow::buildFieldDefsJson() const
{
    QJsonArray arr;
    for (const FieldSpec &sp : m_schema.fields()) {
        QJsonObject o;
        o.insert("key", sp.key);
        o.insert("label", sp.label);
        o.insert("unit", sp.unit);
        o.insert("group", sp.group);
        o.insert("decimals", sp.decimals);
        o.insert("isBool", sp.isBool);
        arr.append(o);
    }
    return arr;
}

// 导出可读(缩进)JSON: <基名>_<类型>.pretty.json
void MainWindow::exportReadableJson()
{
    if (!m_fileSink) {
        appendSystemLine("导出失败: 记录出口不可用");
        return;
    }
    QString err;
    if (m_fileSink->exportPretty(&err)) {
        appendSystemLine("已导出可读 JSON: " + m_fileSink->basePath() +
                         "_<类型>.pretty.json");
    } else {
        appendSystemLine("导出可读 JSON 失败: " + err);
    }
    updateRecordControls();
}

// =====================================================================
// VBO 导出: 把记录(默认取本次记录文件全部样本)写成 VBOX 文本格式
// 结构: File created on + [header] + [channel units] + [comments] +
//       [column names] + [data]
// 说明: 时间基为设备 ts(去回卷后相对首样本的秒); 无 ts 用主机时间;
//       某行缺某通道时填 0(不改变行数, 保持与时间对齐)
// =====================================================================
bool MainWindow::gatherExportRecords(QList<TelemetryRecord> *out,
                                     QString *err) const
{
    out->clear();
    const QString base = m_fileSink ? m_fileSink->basePath() : QString();
    QStringList problems;
    bool anyFile = false;

    if (!base.isEmpty()) {
        static const QStringList types = { "burst", "data", "event", "other" };
        for (const QString &t : types) {
            const QFileInfo fi(base + "/" + t + ".jsonl");
            if (!fi.exists())
                continue;
            anyFile = true;
            QList<QJsonObject> objs;
            QString e;
            if (!loadRecordsFromFile(fi.filePath(), &objs, &e)) {
                problems << e;
                continue;
            }
            for (const QJsonObject &src : objs) {
                TelemetryRecord rec;
                rec.fields = src;
                const QJsonValue seqv = rec.fields.take("seq");
                const QJsonValue linev = rec.fields.take("line_seq");
                const QJsonValue hv = rec.fields.take("host_time");
                rec.ordinal = seqv.isDouble() ? quint64(seqv.toDouble())
                                              : quint64(out->size() + 1);
                rec.lineSeq = linev.isDouble() ? quint64(linev.toDouble())
                                               : rec.ordinal;
                QDateTime ht = parseHostTimeString(hv.toString());
                if (!ht.isValid()) {
                    const QJsonValue tsv = rec.fields.value("ts");
                    ht = tsv.isDouble()
                             ? QDateTime::fromMSecsSinceEpoch(
                                   qint64(tsv.toDouble()))
                             : QDateTime::currentDateTime();
                }
                rec.hostTime = ht;
                out->append(rec);
            }
        }
    }

    if (anyFile && !out->isEmpty())
        return true;

    // 没有记录文件: 退回内存缓冲(曲线/记录表), 会丢失更早样本
    if (out->isEmpty()) {
        *out = m_curveSamples;
        if (out->isEmpty())
            *out = m_historySamples;
    }
    if (out->isEmpty()) {
        if (err)
            *err = "没有可导出的记录文件或样本";
        return false;
    }
    if (err)
        *err = "未找到记录文件, 使用内存中最近样本(更早的数据已丢弃)";
    if (!problems.isEmpty())
        *err += "; " + problems.join("; ");
    return true;
}

static QString vboNum(double v)
{
    QString s = QString::number(v, 'f', 6);
    while (s.contains('.') && (s.endsWith('0')))
        s.chop(1);
    if (s.endsWith('.'))
        s.chop(1);
    return s.isEmpty() ? QStringLiteral("0") : s;
}

static QString vboAsciiUnit(QString u)
{
    // VBO 是欧美工具常用文本, 单位里避免非 ASCII(℃/·等)导致解析问题
    u.replace(QStringLiteral("℃"), QStringLiteral("C"));
    u.replace(QStringLiteral("·"), QStringLiteral("."));
    for (int i = 0; i < u.size(); ++i) {
        if (u.at(i).unicode() > 127)
            u[i] = QLatin1Char('_');
    }
    return u;
}

bool MainWindow::writeVboFile(const QList<TelemetryRecord> &recordsIn,
                              const QString &path, QString *err) const
{
    if (recordsIn.isEmpty()) {
        if (err)
            *err = "没有样本";
        return false;
    }

    QList<TelemetryRecord> recs = recordsIn;
    auto tsOf = [](const TelemetryRecord &r) {
        const QJsonValue v = r.fields.value("ts");
        return v.isDouble() ? v.toDouble() : -1.0;
    };
    std::stable_sort(recs.begin(), recs.end(),
                     [&](const TelemetryRecord &a, const TelemetryRecord &b) {
                         if (a.hostTime.isValid() && b.hostTime.isValid() &&
                             a.hostTime != b.hostTime)
                             return a.hostTime < b.hostTime;
                         return tsOf(a) < tsOf(b);
                     });

    // ---- 通道集合: schema 顺序出现的数值字段(布尔按0/1) + 动态数值字段 ----
    QStringList keys;
    QHash<QString, QString> unitByKey;
    auto addKey = [&](const QString &k) {
        if (k == "type" || k == "ts" || k == "seq" || k == "line_seq" ||
            k == "host_time" || k == "frames")
            return;
        if (keys.contains(k))
            return;
        for (const TelemetryRecord &r : recs) {
            if (r.fields.value(k).isDouble()) {
                keys << k;
                const FieldSpec *sp = m_schema.find(k);
                unitByKey.insert(k, sp ? vboAsciiUnit(sp->unit) : QString());
                return;
            }
        }
    };
    for (const FieldSpec &sp : m_schema.fields())
        addKey(sp.key);
    for (const TelemetryRecord &r : recs) {
        for (auto it = r.fields.constBegin(); it != r.fields.constEnd(); ++it)
            if (it.value().isDouble())
                addKey(it.key());
    }

    // ---- 每行时间(秒): ts 去回卷相对首样本; 无 ts 用主机时间 ----
    double tsBase = -1, tsOffset = 0, tsLast = -1;
    QDateTime hostBase;
    for (const TelemetryRecord &r : recs) {
        if (hostBase.isNull() && r.hostTime.isValid())
            hostBase = r.hostTime;
    }
    QVector<double> times;
    times.reserve(recs.size());
    for (const TelemetryRecord &r : recs) {
        const QJsonValue tv = r.fields.value("ts");
        if (tv.isDouble()) {
            double v = tv.toDouble();
            if (tsLast >= 0 && v + tsOffset < tsLast - 8388608.0)
                tsOffset += 16777216.0;
            v += tsOffset;
            if (tsBase < 0)
                tsBase = v;
            tsLast = v;
            times.append((v - tsBase) / 1000.0);
        } else {
            const double t =
                (hostBase.isValid() && r.hostTime.isValid())
                    ? hostBase.msecsTo(r.hostTime) / 1000.0
                    : 0.0;
            times.append(t);
        }
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = "无法写入 " + path + " : " + f.errorString();
        return false;
    }
    QTextStream out(&f);

    out << "File created on "
        << QDateTime::currentDateTime().toString("dd/MM/yyyy @ HH:mm:ss")
        << "\n\n";

    out << "[header]\n";
    for (const QString &k : keys) {
        const QString u = unitByKey.value(k);
        out << k << (u.isEmpty() ? QString() : " " + u) << "\n";
    }

    out << "\n[channel units]\n";
    for (const QString &k : keys)
        out << unitByKey.value(k) << "\n";

    out << "\n[comments]\n"
        << "version: SerialJsonHost\n"
        << "(c) FSEC 2026\n"
        << "source: " << (m_fileSink ? m_fileSink->basePath() : QString())
        << "\n\n";

    out << "[column names]\n" << keys.join(' ') << "\n\n";
    out << "[data]\n";

    for (int i = 0; i < recs.size(); ++i) {
        out << QString::number(times.at(i), 'f', 2) << ' ';
        for (const QString &k : keys) {
            const QJsonValue v = recs.at(i).fields.value(k);
            out << vboNum(v.isDouble() ? v.toDouble() : 0.0) << ' ';
        }
        out << "\n";
    }
    out.flush();
    f.close();

    if (out.status() != QTextStream::Ok) {
        if (err)
            *err = "写入 VBO 时发生错误";
        return false;
    }
    return true;
}

void MainWindow::exportVbo()
{
    QList<TelemetryRecord> recs;
    QString note;
    if (!gatherExportRecords(&recs, &note)) {
        appendSystemLine("导出 VBO 失败: " + note);
        return;
    }
    if (!note.isEmpty())
        appendSystemLine("导出 VBO 提示: " + note);

    const QString base = m_fileSink ? m_fileSink->basePath() : QString();
    const QString def = base.isEmpty()
                            ? defaultRecordDir() + "/SerialVBO_" +
                                  QDateTime::currentDateTime().toString(
                                      "yyyyMMdd_HHmmss") +
                                  ".vbo"
                            : base + "/export.vbo";
    const QString path = QFileDialog::getSaveFileName(
        this, "导出 VBOX(.vbo) 数据文件", def,
        "VBOX 数据 (*.vbo);;所有文件 (*.*)");
    if (path.isEmpty())
        return;

    QString err;
    if (!writeVboFile(recs, path, &err)) {
        appendSystemLine("导出 VBO 失败: " + err);
        return;
    }
    appendSystemLine(QString("已导出 VBO: %1 (%2 条样本)")
                         .arg(path)
                         .arg(recs.size()));
    statusBar()->showMessage("VBO 导出完成: " + path, 8000);
}

// =====================================================================
// 导入查看: 读取之前保存的记录文件, 灌入同一套显示缓存后重建界面
// (不经过记录出口, 因此不会写回任何文件)
// =====================================================================
bool MainWindow::loadRecordsFromFile(const QString &path,
                                     QList<QJsonObject> *out,
                                     QString *err) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = "无法打开(" + f.errorString() + ")";
        return false;
    }
    const QByteArray data = f.readAll();
    f.close();

    const QByteArray trimmed = data.trimmed();
    if (trimmed.isEmpty()) {
        if (err)
            *err = "空文件";
        return false;
    }

    // ① 整体是一个 JSON(缩进数组 / 单对象 / meta)
    if (trimmed.startsWith('[') || trimmed.startsWith('{')) {
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &pe);
        if (pe.error == QJsonParseError::NoError) {
            if (doc.isArray()) {
                const QJsonArray arr = doc.array();
                for (const QJsonValue &v : arr) {
                    if (v.isObject())
                        out->append(v.toObject());
                }
                return true;
            }
            if (doc.isObject()) {
                const QJsonObject o = doc.object();
                if (o.contains("counts_by_type") || o.contains("fields"))
                    return true; // meta: 不含样本
                out->append(o);
                return true;
            }
        }
        // 解析失败 -> 当作 JSONL 逐行处理
    }

    // ② JSONL 逐行
    int bad = 0;
    const QList<QByteArray> lines = data.split('\n');
    for (QByteArray line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QJsonDocument d = QJsonDocument::fromJson(line);
        if (d.isObject())
            out->append(d.object());
        else
            ++bad;
    }
    if (bad > 0 && err)
        *err = QString("有 %1 行无法解析(已跳过)").arg(bad);
    return true;
}

void MainWindow::applyImportedRecords(const QList<QJsonObject> &objs)
{
    quint64 autoSeq = 0;
    quint64 lastOrdinal = 0;
    const int maxPts = m_plotMaxSpin ? m_plotMaxSpin->value() : 1000;

    for (const QJsonObject &src : objs) {
        // 未知字段自动注册(与实时接收一致, 保证能看到导入内容)
        for (auto kit = src.constBegin(); kit != src.constEnd(); ++kit) {
            const QString key = kit.key();
            if (m_schema.find(key) || isRecordMetaKey(key))
                continue;
            if (!m_fieldOrder.contains(key))
                registerField(key, nullptr);
        }

        TelemetryRecord rec;
        rec.fields = src;
        const QJsonValue seqv = rec.fields.take("seq");
        const QJsonValue linev = rec.fields.take("line_seq");
        const QJsonValue hv = rec.fields.take("host_time");

        ++autoSeq; // 默认连续编号
        quint64 ordinal = autoSeq;
        if (seqv.isDouble()) {
            const quint64 ext = quint64(seqv.toDouble());
            // 旧记录文件 seq 可能全是 0 / 重复: 此时退回连续编号
            if (ext > lastOrdinal) {
                ordinal = ext;
                autoSeq = ext;
            }
        }
        lastOrdinal = ordinal;
        rec.ordinal = ordinal;
        rec.lineSeq = linev.isDouble() ? quint64(linev.toDouble()) : rec.ordinal;

        QDateTime ht = parseHostTimeString(hv.toString());
        if (!ht.isValid()) {
            // 没有主机时刻: 用设备 ts 当毫秒时间戳(相对间隔仍正确)
            const QJsonValue tsv = rec.fields.value("ts");
            ht = tsv.isDouble()
                     ? QDateTime::fromMSecsSinceEpoch(qint64(tsv.toDouble()))
                     : QDateTime::currentDateTime();
        }
        rec.hostTime = ht;

        m_historySamples.append(rec);
        m_curveSamples.append(rec);
        for (auto it = rec.fields.constBegin(); it != rec.fields.constEnd();
             ++it)
            m_lastValue.insert(it.key(), it.value());
    }

    while (m_historySamples.size() > kMaxHistoryRows)
        m_historySamples.removeFirst();
    while (m_curveSamples.size() > maxPts)
        m_curveSamples.removeFirst();

    // 导入走"批量重建", 不经过上屏队列
    m_pendingUi.clear();
    m_latestChanged.clear();

    refreshLatestValues();
    updateLedPanel();
    rebuildHistoryTable();    // 按当前可见字段重放全部导入样本
    rebuildPlotFromHistory(); // 按当前 X 轴/布局/滤波设置重建曲线
}

void MainWindow::importRecords()
{
    QSettings settings;
    const QString dir = settings.value("importDir", defaultRecordDir()).toString();
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        "导入记录文件回看(可多选)",
        dir,
        "记录数据 (*.jsonl *.json);;所有文件 (*.*)");
    if (files.isEmpty())
        return;
    settings.setValue("importDir", QFileInfo(files.first()).absolutePath());

    // 已有数据时询问: 清空后导入 / 追加导入 / 取消
    bool clearFirst = true;
    if (!m_historySamples.isEmpty()) {
        const auto ans = QMessageBox::question(
            this, "导入查看",
            "导入前是否清空当前遥测显示?\n\n"
            "是 = 清空后导入(推荐, 查看某次记录)\n"
            "否 = 追加到当前显示之后",
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
            QMessageBox::Yes);
        if (ans == QMessageBox::Cancel)
            return;
        clearFirst = (ans == QMessageBox::Yes);
    }

    QList<QJsonObject> all;
    QStringList problems;
    QStringList metaInfos;
    int fileCount = 0;

    for (const QString &f : files) {
        // meta 文件: 只提取会话摘要, 不含样本
        const QString fn = QFileInfo(f).fileName();
        if (fn.compare("meta.json", Qt::CaseInsensitive) == 0 ||
            fn.endsWith("_meta.json", Qt::CaseInsensitive)) {
            QFile mf(f);
            if (mf.open(QIODevice::ReadOnly)) {
                const QJsonDocument d = QJsonDocument::fromJson(mf.readAll());
                if (d.isObject()) {
                    const QJsonObject o = d.object();
                    metaInfos << QString("meta[%1]: 端口 %2@%3, 起止 %4 ~ %5, 总数 %6")
                                     .arg(QFileInfo(f).fileName(),
                                          o.value("serial_port").toString(),
                                          QString::number(o.value("baud").toInt()),
                                          o.value("started").toString(),
                                          o.value("ended").toString(),
                                          QString::number(o.value("total_samples").toInt()));
                }
            }
            continue;
        }

        QList<QJsonObject> objs;
        QString err;
        if (!loadRecordsFromFile(f, &objs, &err)) {
            problems << QFileInfo(f).fileName() + ": " + err;
            continue;
        }
        if (!err.isEmpty())
            problems << QFileInfo(f).fileName() + ": " + err;
        if (!objs.isEmpty()) {
            all += objs;
            ++fileCount;
        }
    }

    if (all.isEmpty()) {
        appendSystemLine("导入: 没有读到任何样本" +
                         (problems.isEmpty() ? QString()
                                             : "(" + problems.join("; ") + ")"));
        return;
    }

    if (clearFirst)
        clearTelemetryView(); // 含曲线缓冲与 LED 复位

    applyImportedRecords(all);

    appendSystemLine(QString("导入完成: %1 条样本, 来自 %2 个文件")
                         .arg(all.size())
                         .arg(fileCount));
    for (const QString &m : metaInfos)
        appendSystemLine("导入会话信息 " + m);
    for (const QString &p : problems)
        appendSystemLine("导入提示: " + p);
    statusBar()->showMessage(
        QString("已导入 %1 条样本(仅供查看, 不写入记录)").arg(all.size()), 8000);
}

// =====================================================================
// 状态/标志 LED 常显面板(界面右侧)
// =====================================================================
QWidget *MainWindow::buildLedPanel()
{
    QWidget *panel = new QWidget(this);
    QVBoxLayout *pl = new QVBoxLayout(panel);
    pl->setContentsMargins(0, 0, 0, 0);

    QGroupBox *gb = new QGroupBox("状态 / 标志指示", panel);
    QVBoxLayout *gbv = new QVBoxLayout(gb);
    gbv->setSpacing(2);

    bool any = false;
    for (const FieldSpec &f : m_schema.fields()) {
        if (!f.isBool)
            continue;

        QWidget *row = new QWidget(gb);
        QHBoxLayout *rh = new QHBoxLayout(row);
        rh->setContentsMargins(8, 1, 8, 1);
        auto *lamp = new QLabel("●", row);
        lamp->setStyleSheet("color:#969696;font-size:16px;");
        auto *name = new QLabel(f.label, row);
        name->setToolTip("JSON键: " + f.key +
                         (f.activeIsBad
                              ? "   0=正常(绿) 1=异常/冲突(红)"
                              : "   0=掉线(红) 1=在线(绿)"));
        rh->addWidget(lamp);
        rh->addWidget(name, 1);
        gbv->addWidget(row);

        m_ledLamps.insert(f.key, lamp);
        any = true;
    }
    if (!any)
        gbv->addWidget(new QLabel("(暂无布尔状态字段)", gb));

    pl->addWidget(gb);
    pl->addStretch(1);
    panel->setMinimumWidth(190);
    panel->setMaximumWidth(260);
    return panel;
}

void MainWindow::updateLedPanel()
{
    for (auto it = m_ledLamps.constBegin(); it != m_ledLamps.constEnd();
         ++it) {
        QLabel *lamp = it.value();
        if (!lamp)
            continue;

        const QJsonValue v = m_lastValue.value(it.key());
        const FieldSpec *sp = m_schema.find(it.key());
        const bool bad = sp && sp->activeIsBad;
        QColor c(150, 150, 150);
        QString tip = "无数据(灰)";
        int state = 0;
        if (!v.isUndefined() && !v.isNull()) {
            const bool on = v.isBool()
                                ? v.toBool()
                                : (v.isDouble() && v.toDouble() != 0.0);
            if (bad) {
                // 置位即异常: 1=红, 0=绿
                c = on ? QColor(230, 30, 30) : QColor(0, 160, 60);
                tip = on ? "异常: 冲突/不可靠(红)" : "正常(绿)";
                state = on ? 2 : 1;
            } else {
                c = on ? QColor(0, 160, 60) : QColor(230, 30, 30);
                tip = on ? "在线 / 有效(绿)" : "掉线 / 无效(红)";
                state = on ? 1 : 2;
            }
        }
        // 状态未变则跳过: setStyleSheet/重绘是主要的空转开销
        if (m_ledState.value(it.key(), -1) == state)
            continue;
        m_ledState.insert(it.key(), state);
        lamp->setStyleSheet(QString("color:%1;font-size:16px;").arg(c.name()));
        lamp->setToolTip(tip);
    }
}

// =====================================================================
// 曲线页: Y=数据, X 可选 序号/设备时间/主机时间; 模式 原始/后处理(预留)
// =====================================================================
QWidget *MainWindow::buildPlotTab()
{
    QWidget *page = new QWidget(this);
    QHBoxLayout *hl = new QHBoxLayout(page);
    hl->setContentsMargins(4, 4, 4, 4); 

    // ---- 左侧控制区 ----
    QWidget *ctrl = new QWidget(page);
    QVBoxLayout *cv = new QVBoxLayout(ctrl);
    cv->setContentsMargins(0, 0, 6, 0);

    QGroupBox *yg = new QGroupBox("Y 数据(勾选要画的字段)", ctrl);
    QVBoxLayout *yl = new QVBoxLayout(yg);
    m_plotFieldList = new QListWidget(yg);
    m_plotFieldList->setToolTip("勾选要在曲线上显示的数值字段(可多选)");
    connect(m_plotFieldList, &QListWidget::itemChanged,
            this, &MainWindow::onPlotFieldChanged);
    yl->addWidget(m_plotFieldList);
    cv->addWidget(yg, 1);
    populatePlotFields();

    QGroupBox *xg = new QGroupBox("X 轴(横坐标)", ctrl);
    QHBoxLayout *xl = new QHBoxLayout(xg);
    m_plotXCombo = new QComboBox(xg);
    m_plotXCombo->addItems({ "样本序号", "设备时间 ts (ms)",
                             "主机时间(相对秒)" });
    m_plotXCombo->setToolTip("切换只重画同一批缓冲点, 不丢失/不重算数据");
    connect(m_plotXCombo, &QComboBox::currentIndexChanged,
            this, &MainWindow::onPlotXChanged);
    xl->addWidget(m_plotXCombo, 1);
    cv->addWidget(xg);

    QGroupBox *lg = new QGroupBox("布局", ctrl);
    QHBoxLayout *ll = new QHBoxLayout(lg);
    m_plotLayoutCombo = new QComboBox(lg);
    m_plotLayoutCombo->addItems({ "同图叠加(在一张图)", "分图(每个字段一张)" });
    connect(m_plotLayoutCombo, &QComboBox::currentIndexChanged,
            this, &MainWindow::onPlotLayoutChanged);
    ll->addWidget(m_plotLayoutCombo, 1);
    cv->addWidget(lg);

    QGroupBox *sg = new QGroupBox("纵轴量程", ctrl);
    QHBoxLayout *sl = new QHBoxLayout(sg);
    m_plotScaleCombo = new QComboBox(sg);
    m_plotScaleCombo->addItems({ "共用一个量程", "每条曲线独立量程" });
    m_plotScaleCombo->setToolTip(
        "仅用于\"同图叠加\":\n"
        "共用 = 所有曲线同一条 Y 轴;\n"
        "独立 = 每条曲线各自量程: 第1条刻度在左、第2条在右"
        "(刻度颜色与曲线一致, 不额外占宽度),\n"
        "第3条起左右交替;\n"
        "(\"分图\"模式每张子图固定按自身数据独立量程)");
    connect(m_plotScaleCombo, &QComboBox::currentIndexChanged,
            this, &MainWindow::onPlotScaleChanged);
    sl->addWidget(m_plotScaleCombo, 1);
    cv->addWidget(sg);

    QGroupBox *mg = new QGroupBox("处理模式", ctrl);
    QHBoxLayout *ml = new QHBoxLayout(mg);
    m_plotModeCombo = new QComboBox(mg);
    m_plotModeCombo->addItem("原始(不过滤)");
    m_plotModeCombo->addItem("一阶低通滤波");
    m_plotModeCombo->setToolTip("低通用于抑制高频抖动, 平滑显示");
    connect(m_plotModeCombo, &QComboBox::currentIndexChanged,
            this, &MainWindow::onPlotModeChanged);
    ml->addWidget(m_plotModeCombo, 1);

    m_plotTauSpin = new QSpinBox(mg);
    m_plotTauSpin->setRange(1, 100000);
    m_plotTauSpin->setValue(1000);
    m_plotTauSpin->setSuffix(" ms");
    m_plotTauSpin->setEnabled(false);
    m_plotTauSpin->setToolTip(
        "一阶低通时间常数 τ(ms), 手动可调\n"
        "滤波: y[n] = y[n-1] + α·(x[n]-y[n-1]),  α = 1 - e^(-Δt/τ)\n"
        "τ 越大越平滑但滞后越大");
    connect(m_plotTauSpin, &QSpinBox::valueChanged, this,
            [this](int) {
                resetPlotFilters();
                rebuildPlotFromHistory();
            });
    ml->addWidget(m_plotTauSpin);
    cv->addWidget(mg);

    // ---- 显示范围过滤(临时, 仅影响曲线) ----
    QGroupBox *rg = new QGroupBox("显示范围(看时可改)", ctrl);
    QVBoxLayout *rv = new QVBoxLayout(rg);

    QHBoxLayout *r1 = new QHBoxLayout;
    m_plotRangeChk = new QCheckBox("启用", rg);
    m_plotRangeChk->setToolTip(
        "勾选后, 下方所选字段只显示 [最小, 最大] 范围内的样本;\n"
        "范围外的样本直接不画(曲线在该处断开), 纵轴也不会被拉高。\n"
        "这是临时显示过滤: 不改 fields.json, 不影响记录与异常统计。");
    r1->addWidget(m_plotRangeChk);
    m_plotRangeFieldCombo = new QComboBox(rg);
    m_plotRangeFieldCombo->setToolTip("选择要设置显示范围的字段(只列已勾选的 Y 字段)");
    r1->addWidget(m_plotRangeFieldCombo, 1);
    rv->addLayout(r1);

    QHBoxLayout *r2 = new QHBoxLayout;
    m_plotRangeMinSpin = new QDoubleSpinBox(rg);
    m_plotRangeMinSpin->setRange(-1e9, 1e9);
    m_plotRangeMinSpin->setDecimals(3);
    m_plotRangeMinSpin->setPrefix("min ");
    m_plotRangeMaxSpin = new QDoubleSpinBox(rg);
    m_plotRangeMaxSpin->setRange(-1e9, 1e9);
    m_plotRangeMaxSpin->setDecimals(3);
    m_plotRangeMaxSpin->setPrefix("max ");
    r2->addWidget(m_plotRangeMinSpin);
    r2->addWidget(m_plotRangeMaxSpin);
    rv->addLayout(r2);

    QHBoxLayout *r3 = new QHBoxLayout;
    QPushButton *rangeApplyBtn = new QPushButton("应用到该字段", rg);
    QPushButton *rangeClearBtn = new QPushButton("清除该字段", rg);
    r3->addWidget(rangeApplyBtn);
    r3->addWidget(rangeClearBtn);
    rv->addLayout(r3);

    cv->addWidget(rg);

    connect(m_plotRangeChk, &QCheckBox::toggled, this,
            [this](bool) { rebuildPlotFromHistory(); });
    connect(m_plotRangeFieldCombo, &QComboBox::currentIndexChanged, this,
            [this](int) { populatePlotRangeFields(); }); // 换字段时填回已设范围
    connect(rangeApplyBtn, &QPushButton::clicked, this, [this] {
        const QString key = m_plotRangeFieldCombo->currentData().toString();
        if (key.isEmpty()) {
            appendSystemLine("显示范围: 请先勾选并选择一个 Y 字段");
            return;
        }
        const double lo = m_plotRangeMinSpin->value();
        const double hi = m_plotRangeMaxSpin->value();
        if (!(hi > lo)) {
            appendSystemLine("显示范围无效: 最大值必须大于最小值");
            return;
        }
        m_plotDisplayRange.insert(key, qMakePair(lo, hi));
        if (m_plotRangeChk && !m_plotRangeChk->isChecked())
            m_plotRangeChk->setChecked(true); // 触发重建
        else
            rebuildPlotFromHistory();
        appendSystemLine(QString("曲线显示范围: %1 ∈ [%2, %3]（仅影响显示）")
                             .arg(key)
                             .arg(lo)
                             .arg(hi));
    });
    connect(rangeClearBtn, &QPushButton::clicked, this, [this] {
        const QString key = m_plotRangeFieldCombo->currentData().toString();
        if (key.isEmpty())
            return;
        m_plotDisplayRange.remove(key);
        rebuildPlotFromHistory();
        appendSystemLine("已清除该字段的显示范围: " + key);
    });

    QGroupBox *pg = new QGroupBox("点数上限(每条曲线)", ctrl);
    QHBoxLayout *plg = new QHBoxLayout(pg);
    m_plotMaxSpin = new QSpinBox(pg);
    m_plotMaxSpin->setRange(100, 10000);
    m_plotMaxSpin->setValue(1000);
    m_plotMaxSpin->setToolTip("每条曲线保留的最大点数(同时是曲线缓冲长度)");
    connect(m_plotMaxSpin, &QSpinBox::valueChanged, this,
            [this](int) { rebuildPlotFromHistory(); });
    plg->addWidget(m_plotMaxSpin, 1);
    cv->addWidget(pg);

    QPushButton *clearBtn = new QPushButton("清空曲线", ctrl);
    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::clearPlot);
    cv->addWidget(clearBtn);

    QPushButton *zoomResetBtn = new QPushButton("重置缩放", ctrl);
    zoomResetBtn->setToolTip(
        "在图上按住左键框选可放大局部; 点此恢复自动量程");
    connect(zoomResetBtn, &QPushButton::clicked,
            this, &MainWindow::resetPlotZoom);
    cv->addWidget(zoomResetBtn);

    // ---- 右侧: 绘图区(单图 / 分图两个页面叠放) ----
    m_plotArea = new QStackedWidget(page);

    // 页 0: 同图(唯一 QChartView)
    auto *page0 = new QWidget(m_plotArea);
    auto *p0l = new QVBoxLayout(page0);
    p0l->setContentsMargins(0, 0, 0, 0);
    auto *chart = new QChart();
    chart->legend()->setVisible(true);
    chart->setTitle("遥测曲线(等待数据… 在左侧勾选 Y 字段)");
    m_chartView = new QChartView(chart, page0);
    m_chartView->setRenderHint(QPainter::Antialiasing);
    m_chartView->setMinimumWidth(420);
    m_chartView->setMouseTracking(true); // 悬停点显示数值
    // 框选放大: 选中后暂停自动量程, 点"重置缩放"恢复
    m_chartView->setRubberBand(QChartView::RectangleRubberBand);
    connect(m_chartView, &QChartView::rubberBandChanged, this,
            [this](QRect rect, QPointF, QPointF) {
                if (!rect.isNull())
                    m_plotZoomed = true;
            });
    p0l->addWidget(m_chartView);
    m_plotArea->addWidget(page0);

    // 页 1: 分图(滚动容器, 每字段一张小图)
    m_subplotScroll = new QScrollArea(m_plotArea);
    m_subplotScroll->setWidgetResizable(true);
    m_subplotHost = new QWidget(m_subplotScroll);
    m_subplotHost->setObjectName("SubplotHost");
    m_subplotHost->setLayout(new QVBoxLayout(m_subplotHost));
    m_subplotScroll->setWidget(m_subplotHost);
    m_plotArea->addWidget(m_subplotScroll);

    ctrl->setMinimumWidth(230);
    ctrl->setMaximumWidth(300);
    hl->addWidget(ctrl);
    hl->addWidget(m_plotArea, 1);

    // 定时器: 合并坐标范围刷新, 避免每来一个点就全量扫描(卡顿)
    m_plotAxisTimer = new QTimer(page);
    m_plotAxisTimer->setInterval(200);
    connect(m_plotAxisTimer, &QTimer::timeout, this, [this] {
        if (m_plotDirty) {
            m_plotDirty = false;
            updatePlotAxes();
        }
    });
    m_plotAxisTimer->start();
    populatePlotRangeFields(); // 初始字段下拉
    return page;
}

void MainWindow::populatePlotFields()
{
    if (!m_plotFieldList)
        return;

    m_plotFieldList->clear();
    // 数值字段候选: 非布尔、非包信息键(传感器/信号强度等)
    for (const FieldSpec &f : m_schema.fields()) {
        if (f.isBool)
            continue;
        if (f.key == "type" || f.key == "ts" || f.key == "config_id")
            continue;
        QString text = f.label + (f.unit.isEmpty() ? QString()
                                                   : " [" + f.unit + "]");
        auto *item = new QListWidgetItem(text, m_plotFieldList);
        item->setData(Qt::UserRole, f.key);
        item->setToolTip("JSON键: " + f.key);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
}

// 显示范围过滤的字段下拉: 只列当前勾选的 Y 字段; 并回填该字段已设范围
void MainWindow::populatePlotRangeFields()
{
    if (!m_plotRangeFieldCombo || !m_plotFieldList)
        return;

    const QString cur = m_plotRangeFieldCombo->currentData().toString();
    m_plotRangeFieldCombo->blockSignals(true);
    m_plotRangeFieldCombo->clear();
    for (int i = 0; i < m_plotFieldList->count(); ++i) {
        QListWidgetItem *it = m_plotFieldList->item(i);
        if (!it || it->checkState() != Qt::Checked)
            continue;
        m_plotRangeFieldCombo->addItem(it->text(), it->data(Qt::UserRole));
    }
    int idx = m_plotRangeFieldCombo->findData(cur);
    if (idx < 0 && m_plotRangeFieldCombo->count() > 0)
        idx = 0;
    if (idx >= 0)
        m_plotRangeFieldCombo->setCurrentIndex(idx);
    m_plotRangeFieldCombo->blockSignals(false);

    if (!m_plotRangeMinSpin || !m_plotRangeMaxSpin)
        return;
    const QString key = m_plotRangeFieldCombo->currentData().toString();
    if (key.isEmpty())
        return;
    const auto it = m_plotDisplayRange.constFind(key);
    if (it != m_plotDisplayRange.constEnd()) {
        m_plotRangeMinSpin->setValue(it->first);
        m_plotRangeMaxSpin->setValue(it->second);
        return;
    }
    // 未设过: 默认填入 fields.json 的合理范围, 方便在此基础上改
    if (const FieldSpec *sp = m_schema.find(key)) {
        if (sp->hasRange && sp->maxValue > sp->minValue) {
            m_plotRangeMinSpin->setValue(sp->minValue);
            m_plotRangeMaxSpin->setValue(sp->maxValue);
        } else {
            m_plotRangeMinSpin->setValue(0.0);
            m_plotRangeMaxSpin->setValue(100.0);
        }
    }
}

// X 轴取值: 0=样本序号 1=设备ts(ms) 2=主机相对秒
double MainWindow::plotX(const TelemetryRecord &rec)
{
    const int mode = m_plotXCombo ? m_plotXCombo->currentIndex() : 0;
    if (mode == 1) {
        const QJsonValue ts = rec.fields.value("ts");
        if (!ts.isDouble())
            return std::numeric_limits<double>::quiet_NaN();
        double v = ts.toDouble();
        // 24 位设备时间回卷(约 4.66h): 向下跳超过半量程视为回卷, 累加偏移
        if (m_tsLast >= 0.0 && v + m_tsOffset < m_tsLast - 8388608.0)
            m_tsOffset += 16777216.0;
        v += m_tsOffset;
        m_tsLast = v;
        return v;
    }
    if (mode == 2) {
        if (m_plotT0.isValid())
            return m_plotT0.msecsTo(rec.hostTime) / 1000.0;
        return 0.0;
    }
    return double(rec.ordinal);
}

// 滤波参考时刻(ms): 优先设备 ts, 缺失时用主机时刻
double MainWindow::recordTimeMs(const TelemetryRecord &rec) const
{
    const QJsonValue ts = rec.fields.value("ts");
    if (ts.isDouble())
        return ts.toDouble();
    return double(rec.hostTime.toMSecsSinceEpoch());
}

bool MainWindow::lowpassEnabled() const
{
    return m_plotModeCombo && m_plotModeCombo->currentIndex() == 1;
}

void MainWindow::resetPlotFilters()
{
    m_lpState.clear();
}

// 每个描点用的 Y: 原始模式=原值; 低通模式=一阶低通输出(τ 可调)
double MainWindow::plotValueFor(const QString &key,
                                const TelemetryRecord &rec)
{
    const QJsonValue v = rec.fields.value(key);
    if (!v.isDouble())
        return std::numeric_limits<double>::quiet_NaN();
    const double raw = v.toDouble();

    // 曲线显示范围(临时): 用户在看图时手动设定的范围优先于 fields.json,
    // 范围外样本直接不画; 仅影响显示, 不影响记录/异常统计。
    const auto dr = m_plotDisplayRange.constFind(key);
    const bool hasDisplayRange =
        m_plotRangeChk && m_plotRangeChk->isChecked() &&
        dr != m_plotDisplayRange.constEnd();
    if (hasDisplayRange) {
        if (raw < dr->first || raw > dr->second)
            return std::numeric_limits<double>::quiet_NaN();
    } else if (valueOutOfRange(m_schema.find(key), v)) {
        // 未设显示范围时: 仍按 fields.json 的合理范围剔除坏值
        return std::numeric_limits<double>::quiet_NaN();
    }

    if (!lowpassEnabled())
        return raw;

    const double t = recordTimeMs(rec);
    const double tau = m_plotTauSpin
                           ? qMax(1.0, double(m_plotTauSpin->value()))
                           : 1000.0;

    PlotFilterState &st = m_lpState[key];
    if (!st.valid || st.tMs < 0.0) {
        st.y = raw;
        st.tMs = t;
        st.valid = true;
        return raw; // 首点不过滤
    }
    const double dt = t - st.tMs;
    if (dt <= 0.0)
        return st.y; // 时间重复/回退: 保持上次输出, 避免跳变

    const double alpha = 1.0 - std::exp(-dt / tau);
    st.y += alpha * (raw - st.y);
    st.tMs = t;
    return st.y;
}

// 新建一个"连续有效段"曲线: 颜色按字段固定, 不占图例, 悬停提示照常
QLineSeries *MainWindow::newPlotSegment(const QString &key, QChart *chart,
                                        QValueAxis *xAxis, QValueAxis *yAxis,
                                        QWidget *hoverOwner)
{
    if (!chart || !xAxis || !yAxis)
        return nullptr;

    auto *seg = new QLineSeries(chart);
    seg->setPen(QPen(m_plotColorByKey.value(key, QColor(31, 119, 180)), 2));
    chart->addSeries(seg);
    seg->attachAxis(xAxis);
    seg->attachAxis(yAxis);
    // 数据段不显示图例(图例项由每个字段的 legendSeries 占一个)
    for (QLegendMarker *m : chart->legend()->markers(seg))
        m->setVisible(false);
    connect(seg, &QLineSeries::hovered, this,
            [this, key, hoverOwner](const QPointF &pt, bool state) {
                if (!state) {
                    QToolTip::hideText();
                    return;
                }
                showPlotPointTooltip(key, pt, hoverOwner);
            });
    return seg;
}

// 只保留最近 maxPts 个点: 从最旧的段开始裁剪, 段空了就删除
void MainWindow::trimPlotGroup(PlotSeriesGroup &g, int maxPts)
{
    if (maxPts <= 1)
        maxPts = 1;

    // 段数上限: 防止"好坏交替"的异常数据产生上千个曲线段
    static constexpr int kMaxPlotSegments = 64;
    while (g.segments.size() > kMaxPlotSegments) {
        QLineSeries *old = g.segments.takeFirst();
        if (!old)
            continue;
        if (QChart *c = old->chart())
            c->removeSeries(old);
        old->deleteLater();
    }

    int total = 0;
    for (QLineSeries *s : g.segments)
        total += s ? s->count() : 0;

    while (total > maxPts && !g.segments.isEmpty()) {
        QLineSeries *first = g.segments.first();
        if (!first) {
            g.segments.removeFirst();
            continue;
        }
        const int remove = qMin(total - maxPts, first->count());
        if (remove > 0) {
            first->removePoints(0, remove);
            total -= remove;
        }
        if (first->count() == 0) {
            g.segments.removeFirst();
            if (QChart *c = first->chart())
                c->removeSeries(first);
            first->deleteLater();
        }
    }
}

void MainWindow::rebuildPlotFromHistory()
{
    if (!m_chartView || !m_plotArea)
        return;

    // 先把尚未上屏的样本补进曲线缓冲, 再清空队列(避免丢最后一批点)
    flushPendingToCurveBuffer();
    // 重建时从缓冲重新计算滤波链(原始模式忽略此状态)
    resetPlotFilters();
    // 结构重建(切轴/改布局/改字段)时退出框选缩放, 回到自动量程
    m_plotZoomed = false;

    // "主机时间(相对秒)"基准 = 曲线缓冲第一条(必须在计算 X 之前设置)
    if (m_plotXCombo && m_plotXCombo->currentIndex() == 2)
        m_plotT0 = m_curveSamples.isEmpty() ? QDateTime()
                                            : m_curveSamples.first().hostTime;

    // 时间轴去回卷状态复位; 按缓冲顺序统一算一次每个样本的 X
    // (时间轴上事件包可能比burst帧晚发先到, X 会短暂倒退, 绘制时排序处理)
    m_tsLast = -1.0;
    m_tsOffset = 0.0;
    QVector<double> xs;
    xs.reserve(m_curveSamples.size());
    for (const TelemetryRecord &rec : m_curveSamples)
        xs.append(plotX(rec));

    // ---- 收集当前勾选的 Y 字段 ----
    QStringList keys;
    if (m_plotFieldList) {
        for (int i = 0; i < m_plotFieldList->count(); ++i) {
            QListWidgetItem *item = m_plotFieldList->item(i);
            if (item && item->checkState() == Qt::Checked)
                keys << item->data(Qt::UserRole).toString();
        }
    }
    const bool separate = m_plotLayoutCombo &&
                          m_plotLayoutCombo->currentIndex() == 1;
    // 分图模式: 每张子图固定独立量程(纵轴选项仅作用于"同图")
    if (m_plotScaleCombo)
        m_plotScaleCombo->setEnabled(!separate);
    const bool sharedY = !separate &&
                         !(m_plotScaleCombo &&
                           m_plotScaleCombo->currentIndex() == 1);

    static const QList<QColor> palette = {
        QColor(31, 119, 180), QColor(255, 127, 14), QColor(44, 160, 44),
        QColor(214, 39, 40),  QColor(148, 103, 189), QColor(140, 86, 75),
        QColor(227, 119, 194), QColor(127, 127, 127),
    };

    // ---- 清空旧内容(曲线 / 轴 / 分图视图) ----
    m_plotGroups.clear();
    m_plotColorByKey.clear();
    m_axisXByKey.clear();
    m_axisYByKey.clear();

    QChart *singleChart = m_chartView->chart();
    singleChart->removeAllSeries();
    const auto oldAxes = singleChart->axes();
    for (QAbstractAxis *a : oldAxes) {
        singleChart->removeAxis(a);
        delete a;
    }
    for (auto it = m_plotViews.constBegin(); it != m_plotViews.constEnd();
         ++it) {
        if (it.value()) {
            m_subplotHost->layout()->removeWidget(it.value());
            it.value()->deleteLater();
        }
    }
    m_plotViews.clear();

    // 没有勾选字段: 空图返回
    if (keys.isEmpty()) {
        updatePlotAxes();
        m_plotDirty = false;
        return;
    }

    const int xIdx = m_plotXCombo ? m_plotXCombo->currentIndex() : 0;
    const QString xLabelFmt = xIdx == 2 ? QStringLiteral("%.1f")
                                        : QStringLiteral("%.0f");
    const QString xTitle = m_plotXCombo ? m_plotXCombo->currentText()
                                        : QStringLiteral("序号");

    const int maxPts = m_plotMaxSpin ? m_plotMaxSpin->value() : 1000;
    int colorIdx = 0;

    auto configureXAxis = [&](QValueAxis *ax) {
        ax->setTitleText(xTitle);
        ax->setLabelFormat(xLabelFmt);
    };

    if (!separate) {
        // ---------- 同图 ----------
        QValueAxis *sharedX = new QValueAxis(singleChart);
        singleChart->addAxis(sharedX, Qt::AlignBottom);
        configureXAxis(sharedX);

        QValueAxis *sharedY = nullptr;
        if (m_plotScaleCombo && m_plotScaleCombo->currentIndex() == 0) {
            // 共用一个纵轴
            sharedY = new QValueAxis(singleChart);
            singleChart->addAxis(sharedY, Qt::AlignLeft);
            sharedY->setTitleText("数据值");
        }

        int independentIndex = 0; // 独立量程轴序号: 0=左, 1=右, 之后左右交替
        for (int i = 0; i < m_plotFieldList->count(); ++i) {
            QListWidgetItem *item = m_plotFieldList->item(i);
            if (!item || item->checkState() != Qt::Checked)
                continue;
            const QString key = item->data(Qt::UserRole).toString();

            const QColor color = palette.at(colorIdx % palette.size());
            ++colorIdx;
            m_plotColorByKey.insert(key, color);

            QValueAxis *yAxis;
            if (sharedY) {
                yAxis = sharedY;
            } else {
                // 独立量程: 第一条轴放左侧、第二条放右侧(不额外占用宽度),
                // 用与曲线一致的颜色区分; 第三条起左右交替堆叠
                yAxis = new QValueAxis(singleChart);
                const bool leftSide = (independentIndex % 2 == 0);
                singleChart->addAxis(yAxis,
                                     leftSide ? Qt::AlignLeft : Qt::AlignRight);
                yAxis->setTitleText(item->text());
                yAxis->setTitleBrush(QBrush(color));
                yAxis->setLabelsColor(color);
                yAxis->setLinePenColor(color);
                if (independentIndex > 0)
                    yAxis->setGridLineVisible(false); // 只由第一条轴画网格
                ++independentIndex;
            }

            // 图例项: 一条空曲线, 保证每个字段在图中只出现一个图例条目
            auto *legendSeries = new QLineSeries(singleChart);
            legendSeries->setName(item->text());
            legendSeries->setPen(QPen(color, 2));
            singleChart->addSeries(legendSeries);
            legendSeries->attachAxis(sharedX);
            legendSeries->attachAxis(yAxis);

            PlotSeriesGroup group;
            group.legendSeries = legendSeries;

            // 回放缓冲: 连续有效点连成一段, 越界/缺失处断开(不画跨坏点的线)
            QVector<QPointF> run;
            run.reserve(m_curveSamples.size());
            auto flushRun = [&]() {
                if (run.isEmpty())
                    return;
                std::sort(run.begin(), run.end(),
                          [](const QPointF &a, const QPointF &b) {
                              return a.x() < b.x();
                          });
                QLineSeries *seg = newPlotSegment(key, singleChart, sharedX,
                                                  yAxis, m_chartView);
                if (seg) {
                    seg->replace(run);
                    group.segments.append(seg);
                }
                run.clear();
            };
            for (int si = 0; si < m_curveSamples.size(); ++si) {
                const double x = xs.at(si);
                const double y = plotValueFor(key, m_curveSamples.at(si));
                if (qIsNaN(x) || qIsNaN(y)) {
                    flushRun(); // 坏数据处断开
                    group.broken = true;
                    continue;
                }
                run.append(QPointF(x, y));
                group.broken = false;
            }
            flushRun();
            trimPlotGroup(group, maxPts);

            m_plotGroups.insert(key, group);
            m_axisXByKey.insert(key, sharedX);
            m_axisYByKey.insert(key, yAxis);
        }
        // 再统一隐藏一次数据段的图例项(图例只保留每字段的 legendSeries 一条)
        for (const PlotSeriesGroup &g : m_plotGroups) {
            for (QLineSeries *s : g.segments) {
                for (QLegendMarker *m : singleChart->legend()->markers(s))
                    m->setVisible(false);
            }
        }
        singleChart->setTitle("遥测曲线(同图)");
    } else {
        // ---------- 分图: 每个字段一张小图 ----------
        for (int i = 0; i < m_plotFieldList->count(); ++i) {
            QListWidgetItem *item = m_plotFieldList->item(i);
            if (!item || item->checkState() != Qt::Checked)
                continue;
            const QString key = item->data(Qt::UserRole).toString();

            auto *chart = new QChart();
            chart->legend()->setVisible(false);
            chart->setTitle(item->text());
            auto *view = new QChartView(chart, m_subplotHost);
            view->setRenderHint(QPainter::Antialiasing);
            view->setMinimumHeight(200);
            view->setMouseTracking(true);
            // 分图也支持各自框选放大
            view->setRubberBand(QChartView::RectangleRubberBand);
            connect(view, &QChartView::rubberBandChanged, this,
                    [this](QRect rect, QPointF, QPointF) {
                        if (!rect.isNull())
                            m_plotZoomed = true;
                    });

            QValueAxis *ax = new QValueAxis(chart);
            chart->addAxis(ax, Qt::AlignBottom);
            configureXAxis(ax);
            QValueAxis *ay = new QValueAxis(chart);
            chart->addAxis(ay, Qt::AlignLeft);
            ay->setTitleText("数据值");

            const QColor color = palette.at(colorIdx % palette.size());
            ++colorIdx;
            m_plotColorByKey.insert(key, color);

            PlotSeriesGroup group;
            QVector<QPointF> run;
            run.reserve(m_curveSamples.size());
            auto flushRun = [&]() {
                if (run.isEmpty())
                    return;
                std::sort(run.begin(), run.end(),
                          [](const QPointF &a, const QPointF &b) {
                              return a.x() < b.x();
                          });
                QLineSeries *seg = newPlotSegment(key, chart, ax, ay, view);
                if (seg) {
                    seg->replace(run);
                    group.segments.append(seg);
                }
                run.clear();
            };
            for (int si = 0; si < m_curveSamples.size(); ++si) {
                const double x = xs.at(si);
                const double y = plotValueFor(key, m_curveSamples.at(si));
                if (qIsNaN(x) || qIsNaN(y)) {
                    flushRun(); // 坏数据处断开
                    group.broken = true;
                    continue;
                }
                run.append(QPointF(x, y));
                group.broken = false;
            }
            flushRun();
            trimPlotGroup(group, maxPts);

            m_plotGroups.insert(key, group);
            m_plotViews.insert(key, view);
            m_axisXByKey.insert(key, ax);
            m_axisYByKey.insert(key, ay);
            m_subplotHost->layout()->addWidget(view);
        }
    }

    m_plotArea->setCurrentIndex(separate ? 1 : 0);
    updatePlotAxes();
    m_plotDirty = false;
}

void MainWindow::appendPlotPoint(const TelemetryRecord &rec)
{
    Q_UNUSED(rec); // (保留接口占位: 曲线点统一由 onUiTick 批量追加)
}

void MainWindow::updatePlotAxes()
{
    // 用户框选放大期间不要用自动量程覆盖缩放结果
    if (m_plotZoomed)
        return;
    if (m_plotGroups.isEmpty() || m_axisXByKey.isEmpty())
        return;

    const bool separate = m_plotLayoutCombo &&
                          m_plotLayoutCombo->currentIndex() == 1;
    const bool sharedY = !separate &&
                         !(m_plotScaleCombo &&
                           m_plotScaleCombo->currentIndex() == 1);

    // ---- X: 所有曲线(含所有有效段)共用同一个横轴范围 ----
    double x0 = 0, x1 = 0, uy0 = 0, uy1 = 0;
    bool hx = false, hy = false;
    for (const PlotSeriesGroup &g : m_plotGroups) {
        for (QLineSeries *s : g.segments) {
            if (!s)
                continue;
            for (const QPointF &p : s->points()) {
                if (!hx) { x0 = x1 = p.x(); hx = true; }
                else { x0 = qMin(x0, p.x()); x1 = qMax(x1, p.x()); }
                if (sharedY) {
                    if (!hy) { uy0 = uy1 = p.y(); hy = true; }
                    else { uy0 = qMin(uy0, p.y()); uy1 = qMax(uy1, p.y()); }
                }
            }
        }
    }
    if (!hx) {
        for (QValueAxis *ax : m_axisXByKey) ax->setRange(0, 1);
        for (QValueAxis *ay : m_axisYByKey) ay->setRange(0, 1);
        return;
    }
    if (x1 - x0 < 1e-9) { x0 -= 1; x1 += 1; }
    const double dx = (x1 - x0) * 0.02;

    for (auto it = m_plotGroups.constBegin(); it != m_plotGroups.constEnd();
         ++it) {
        const QString &key = it.key();
        QValueAxis *ax = m_axisXByKey.value(key, nullptr);
        QValueAxis *ay = m_axisYByKey.value(key, nullptr);
        if (!ax || !ay)
            continue;
        ax->setRange(x0 - dx, x1 + dx);

        if (sharedY) {
            if (!hy) { ay->setRange(0, 1); continue; }
            double yy0 = uy0, yy1 = uy1;
            if (yy1 - yy0 < 1e-9) { yy0 -= 1; yy1 += 1; }
            const double dy = (yy1 - yy0) * 0.02;
            ay->setRange(yy0 - dy, yy1 + dy);
        } else {
            // 独立量程: 该字段所有有效段的统一范围
            double y0 = 0, y1 = 0;
            bool h = false;
            for (QLineSeries *s : it.value().segments) {
                if (!s)
                    continue;
                for (const QPointF &p : s->points()) {
                    if (!h) { y0 = y1 = p.y(); h = true; }
                    else { y0 = qMin(y0, p.y()); y1 = qMax(y1, p.y()); }
                }
            }
            if (!h) { ay->setRange(0, 1); continue; }
            if (y1 - y0 < 1e-9) { y0 -= 1; y1 += 1; }
            const double dy = (y1 - y0) * 0.02;
            ay->setRange(y0 - dy, y1 + dy);
        }
    }
}

// 从曲线缓冲里取某 X 位置对应的原始值(低通模式提示"原始值"用)
double MainWindow::rawValueAt(const QString &key, double targetX) const
{
    const int mode = m_plotXCombo ? m_plotXCombo->currentIndex() : 0;
    double off = 0.0, last = -1.0;
    double best = std::numeric_limits<double>::max();
    double val = std::numeric_limits<double>::quiet_NaN();

    for (const TelemetryRecord &rec : m_curveSamples) {
        double x = 0.0;
        if (mode == 1) {
            const QJsonValue tv = rec.fields.value("ts");
            if (!tv.isDouble())
                continue;
            double v = tv.toDouble();
            if (last >= 0 && v + off < last - 8388608.0)
                off += 16777216.0;
            v += off;
            last = v;
            x = v;
        } else if (mode == 2) {
            x = m_plotT0.isValid()
                    ? m_plotT0.msecsTo(rec.hostTime) / 1000.0
                    : 0.0;
        } else {
            x = double(rec.ordinal);
        }
        const double d = qAbs(x - targetX);
        if (d < best) {
            const QJsonValue yv = rec.fields.value(key);
            if (yv.isDouble()) {
                best = d;
                val = yv.toDouble();
            }
        }
    }
    if (best > qMax(1e-6, qAbs(targetX) * 1e-9))
        return std::numeric_limits<double>::quiet_NaN();
    return val;
}

// 悬停提示: X(序号/ts/相对秒) + 显示值; 低通时追加原始值
void MainWindow::showPlotPointTooltip(const QString &key, const QPointF &pt,
                                      QWidget *owner)
{
    const FieldSpec *sp = m_schema.find(key);
    const int xi = m_plotXCombo ? m_plotXCombo->currentIndex() : 0;
    QString xText;
    if (xi == 1) {
        xText = QString("设备时间 ts: %1 ms").arg(qint64(pt.x()));
    } else if (xi == 2) {
        xText = QString("主机时间: %1 s").arg(QString::number(pt.x(), 'f', 3));
    } else {
        xText = QString("样本序号: %1").arg(qint64(pt.x()));
    }

    const QString unit = sp ? sp->unit : QString();
    const QString yDisp = formatValue(sp, QJsonValue(pt.y()));
    QStringList lines;
    lines << QString("曲线: %1").arg(sp ? sp->label : key);
    lines << xText;
    lines << QString("显示值: %1%2")
                 .arg(yDisp, unit.isEmpty() ? QString()
                                            : " " + unit);

    if (lowpassEnabled()) {
        const double raw = rawValueAt(key, pt.x());
        if (!qIsNaN(raw)) {
            lines << QString("原始值: %1%2")
                         .arg(formatValue(sp, QJsonValue(raw)),
                              unit.isEmpty() ? QString() : " " + unit);
        }
    }
    QToolTip::showText(QCursor::pos(), lines.join('\n'), owner);
}

void MainWindow::onPlotFieldChanged(QListWidgetItem *item)
{
    Q_UNUSED(item);
    rebuildPlotFromHistory();
    populatePlotRangeFields(); // 勾选变化后同步显示范围字段下拉
}

void MainWindow::onPlotXChanged(int index)
{
    Q_UNUSED(index);
    rebuildPlotFromHistory();
}

void MainWindow::onPlotModeChanged(int index)
{
    if (m_plotTauSpin)
        m_plotTauSpin->setEnabled(index == 1);
    resetPlotFilters();
    rebuildPlotFromHistory();
}

void MainWindow::clearPlot()
{
    // 只清数据点(曲线缓冲), 保留字段勾选与布局设置
    m_curveSamples.clear();
    m_pendingUi.clear();
    rebuildPlotFromHistory();
}

// 退出框选缩放: 恢复自动量程(同图与分图都还原)
void MainWindow::resetPlotZoom()
{
    m_plotZoomed = false;
    if (m_chartView && m_chartView->chart()) {
        m_chartView->chart()->zoomReset();
    }
    for (auto it = m_plotViews.constBegin(); it != m_plotViews.constEnd();
         ++it) {
        if (it.value() && it.value()->chart())
            it.value()->chart()->zoomReset();
    }
    updatePlotAxes();
    m_plotDirty = false;
}

void MainWindow::onPlotLayoutChanged(int index)
{
    Q_UNUSED(index);
    rebuildPlotFromHistory();
}

void MainWindow::onPlotScaleChanged(int index)
{
    Q_UNUSED(index);
    rebuildPlotFromHistory();
}

bool MainWindow::telemetryTabVisible() const
{
    return !m_tabs || m_tabs->currentIndex() == 1;
}

bool MainWindow::curveTabVisible() const
{
    return !m_tabs || m_tabs->currentIndex() == 2;
}

// 把待上屏样本补进曲线缓冲(隐藏曲线页时也保持缓冲完整)
void MainWindow::flushPendingToCurveBuffer()
{
    if (m_pendingUi.isEmpty())
        return;
    const int maxPts = m_plotMaxSpin ? m_plotMaxSpin->value() : 1000;
    for (const TelemetryRecord &rec : m_pendingUi)
        m_curveSamples.append(rec);
    while (m_curveSamples.size() > maxPts)
        m_curveSamples.removeFirst();
    m_pendingUi.clear();
}

// =====================================================================
// 批量上屏(20ms):
//   - 原始页: 批量渲染(避免逐行 appendHtml 阻塞事件循环)
//   - 最新值/记录: 仅"遥测数据"页可见时刷新; 隐藏时切回页签再重建
//   - 曲线: 缓冲始终保存; 只有"曲线"页可见时才真正描点/重绘
// =====================================================================
void MainWindow::onUiTick()
{
    // 0) 原始数据页: 每次上屏最多渲染 300 行
    flushRawPending();

    // 异常计数标签: 批次刷新一次(避免每帧重设样式)
    if (m_anomalyLabelDirty) {
        m_anomalyLabelDirty = false;
        updateAnomalyLabel();
    }

    // 空闲: 没有待上屏数据时直接返回, 不做任何控件操作
    if (m_pendingUi.isEmpty() && m_latestChanged.isEmpty())
        return;

    // 1) 最新值 + LED: 只在前台页且有新数据时刷新(无变化不碰控件)
    if (telemetryTabVisible() && !m_latestChanged.isEmpty()) {
        updateLedPanel();
        if (m_latestTable) {
            m_latestTable->setUpdatesEnabled(false);
            for (const QString &key : m_latestChanged) {
                if (m_hidden.contains(key))
                    continue;
                const int row = m_latestRowByKey.value(key, -1);
                if (row < 0)
                    continue;
                const FieldSpec *sp = m_schema.find(key);
                const QJsonValue v = m_lastValue.value(key);
                QTableWidgetItem *it = m_latestTable->item(row, 1);
                if (!it) {
                    it = new QTableWidgetItem;
                    m_latestTable->setItem(row, 1, it);
                }
                if (sp && sp->isBool) {
                    applyBoolLed(it, sp, v);
                } else {
                    it->setText(v.isUndefined() ? QStringLiteral("—")
                                                : formatValue(sp, v));
                    it->setToolTip(QStringLiteral("JSON键: ") + key);
                }
            }
            m_latestTable->setUpdatesEnabled(true);
        }
    }
    m_latestChanged.clear();

    if (m_pendingUi.isEmpty())
        return;

    // 2) 接收记录: 仅遥测页可见时插入; 隐藏时由切页签时重建补上
    if (telemetryTabVisible() && m_historyModel && m_historyView) {
        m_historyModel->appendRecords(m_pendingUi);
        m_historyView->scrollToBottom();
    }

    // 3) 曲线缓冲: 无论曲线页是否可见都保存(切回来自动重建)
    const int maxPts = m_plotMaxSpin ? m_plotMaxSpin->value() : 1000;
    for (const TelemetryRecord &rec : m_pendingUi)
        m_curveSamples.append(rec);
    while (m_curveSamples.size() > maxPts)
        m_curveSamples.removeFirst();

    // 只有曲线页可见时才描点/重绘(隐藏页的图表更新最耗 CPU)
    if (curveTabVisible() && m_chartView && !m_plotGroups.isEmpty()) {
        if (m_plotXCombo && m_plotXCombo->currentIndex() == 2 &&
            !m_plotT0.isValid() && !m_pendingUi.isEmpty())
            m_plotT0 = m_pendingUi.first().hostTime;

        bool addedAny = false;
        QSet<QString> needSort; // 出现 X 回退的字段(仅段内排序, 不跨段连线)

        for (const TelemetryRecord &rec : m_pendingUi) {
            const double x = plotX(rec);
            if (qIsNaN(x))
                continue;
            for (auto git = m_plotGroups.begin(); git != m_plotGroups.end();
                 ++git) {
                const QString &key = git.key();
                PlotSeriesGroup &g = git.value();
                const double y = plotValueFor(key, rec);
                if (qIsNaN(y)) {
                    g.broken = true; // 越界/缺失: 断开, 不画也不连线
                    continue;
                }
                QValueAxis *ax = m_axisXByKey.value(key, nullptr);
                QValueAxis *ay = m_axisYByKey.value(key, nullptr);
                QChart *chart = nullptr;
                QWidget *owner = m_chartView;
                if (auto *view = m_plotViews.value(key, nullptr)) {
                    chart = view->chart();
                    owner = view;
                } else {
                    chart = m_chartView->chart();
                }
                if (!ax || !ay || !chart) {
                    g.broken = true;
                    continue;
                }
                if (g.broken || g.segments.isEmpty()) {
                    QLineSeries *seg = newPlotSegment(key, chart, ax, ay, owner);
                    if (!seg) {
                        g.broken = true;
                        continue;
                    }
                    g.segments.append(seg);
                    g.broken = false;
                }
                QLineSeries *seg = g.segments.last();
                if (!seg) {
                    g.broken = true;
                    continue;
                }
                if (seg->count() > 0 &&
                    x < seg->at(seg->count() - 1).x() - 1e-9)
                    needSort.insert(key);
                seg->append(x, y);
                addedAny = true;
            }
        }

        // 段内出现 X 回退: 只对受影响的段排序(段与段之间保持断开)
        for (const QString &key : needSort) {
            PlotSeriesGroup &g = m_plotGroups[key];
            for (QLineSeries *s : g.segments) {
                if (!s || s->count() < 2)
                    continue;
                QVector<QPointF> pts = s->points();
                std::stable_sort(pts.begin(), pts.end(),
                                 [](const QPointF &a, const QPointF &b) {
                                     return a.x() < b.x();
                                 });
                s->replace(pts);
            }
        }

        // 裁剪到点数上限(从最旧段开始丢点/丢段)
        for (auto git = m_plotGroups.begin(); git != m_plotGroups.end();
             ++git)
            trimPlotGroup(git.value(), maxPts);

        if (addedAny)
            m_plotDirty = true;
    }

    m_pendingUi.clear();
}
