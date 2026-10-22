#pragma once

#include <QByteArray>
#include <QObject>
#include <QSerialPort>
#include <QString>

// =====================================================================
// SerialReceiver —— 串口采集层
//
// 职责(只负责"收"):
//   1. 打开/关闭串口(QSerialPort, 固定 8 数据位 / 无校验 / 1 停止位,
//      波特率由界面选择, 默认 115200);
//   2. readyRead 到达的字节流累积到内部缓冲, 解决"半包/粘包";
//   3. 按换行符 \n 切出一条条完整行(兼容 \r\n 与 \n), 通过
//      lineReceived(QByteArray) 信号上行交给 JsonFrameParser。
//
// 本类不解析内容 —— 解析属于下一层 JsonFrameParser 的职责。
// =====================================================================
class SerialReceiver : public QObject {
    Q_OBJECT

public:
    explicit SerialReceiver(QObject *parent = nullptr);
    ~SerialReceiver() override;

    bool isOpen() const { return m_port.isOpen(); }
    QString portName() const { return m_port.portName(); }

    // 静态辅助: 枚举当前系统可用串口(名称列表, 如 "COM5")
    static QStringList availablePortNames();

public slots:
    // 打开串口; 失败时返回 false 并通过 errorOccurred() 给出原因(仅报一次)
    bool openPort(const QString &portName, qint32 baudRate);
    void closePort();

signals:
    // 切好的一条完整行(不含行尾的 \r / \n)。时间戳/序号由解析层统一盖。
    void lineReceived(const QByteArray &line);

    void opened();
    void closed();
    void errorOccurred(const QString &message);

private slots:
    void onReadyRead();
    void onSerialError(QSerialPort::SerialPortError error);

private:
    void drainBuffer();

    QSerialPort m_port;
    QByteArray  m_buffer;   // 未切完的半包残留
    bool        m_opening = false; // 正在执行 open(): 期间的错误统一按"打开结果"报告
};
