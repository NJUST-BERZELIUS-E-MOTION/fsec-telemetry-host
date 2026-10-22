#include "core/SerialReceiver.h"

#include <QSerialPortInfo>

SerialReceiver::SerialReceiver(QObject *parent)
    : QObject(parent)
{
    // 串口参数: 8N1(与固件 usart.c 一致)
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);

    connect(&m_port, &QSerialPort::readyRead, this, &SerialReceiver::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, &SerialReceiver::onSerialError);
}

SerialReceiver::~SerialReceiver()
{
    if (m_port.isOpen())
        m_port.close();
}

QStringList SerialReceiver::availablePortNames()
{
    QStringList names;
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports)
        names.append(info.portName());
    return names;
}

bool SerialReceiver::openPort(const QString &portName, qint32 baudRate)
{
    if (m_port.isOpen())
        m_port.close();
    if (portName.isEmpty())
        return false;

    m_buffer.clear();
    m_port.setPortName(portName);
    m_port.setBaudRate(baudRate);

    m_opening = true;
    const bool ok = m_port.open(QIODevice::ReadWrite);
    m_opening = false;

    if (!ok)
        return false; // 失败原因已由 onSerialError() 统一报告(只报一次)

    emit opened();
    return true;
}

void SerialReceiver::closePort()
{
    if (m_port.isOpen()) {
        m_port.close();
        emit closed();
    }
}

void SerialReceiver::onReadyRead()
{
    m_buffer.append(m_port.readAll());
    drainBuffer();
}

void SerialReceiver::drainBuffer()
{
    // 按 '\n' 切行; 兼容 '\r\n' 与裸 '\n' 两种行尾
    int start = 0;
    int newline = -1;
    while ((newline = m_buffer.indexOf('\n', start)) >= 0) {
        // 取出本行并去掉结尾 '\r'
        QByteArray line = m_buffer.mid(start, newline - start);
        if (line.endsWith('\r'))
            line.chop(1);
        start = newline + 1;

        if (line.isEmpty())
            continue;

        emit lineReceived(line);
    }

    // 剩余未切完的部分留作下一轮(半包)
    m_buffer = (start < m_buffer.size()) ? m_buffer.mid(start) : QByteArray();
    if (m_buffer.size() > 64 * 1024)
        m_buffer.clear(); // 防御: 异常无换行的洪流
}

// 打开串口失败时对用户可读、可执行的提示(含常见原因与排查方向)
static QString describeOpenFailure(const QString &portName,
                                   const QString &systemReason)
{
    return "无法打开 " + portName + ": " + systemReason +
           "\n请检查: ① 设备管理器(端口)里该 COM 口是否正常、无黄色感叹号;"
           " ② 是否已被其它串口工具或本程序另一实例占用;"
           " ③ 重新插拔 USB / 复位板子后再试;"
           " ④ 若换工具仍打不开, 多为驱动或硬件问题(本程序无法解决)。";
}

void SerialReceiver::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;

    // open() 执行期间的错误 = 本次打开的结果, 报告一次即可
    if (m_opening) {
        emit errorOccurred(describeOpenFailure(m_port.portName(),
                                               m_port.errorString()));
        return;
    }

    // 常见错误给友好提示; 其余显示原始错误串
    QString text;
    switch (error) {
    case QSerialPort::DeviceNotFoundError:
        text = "设备不存在(端口被拔出或不存在)";
        break;
    case QSerialPort::PermissionError:
        text = "端口被占用/无权限(可能被其它串口工具打开)";
        break;
    case QSerialPort::OpenError:
        text = "打开失败(参数错误或设备忙)";
        break;
    case QSerialPort::ResourceError:
        text = "端口异常断开(设备被拔出?)";
        break;
    case QSerialPort::TimeoutError:
        text = "操作超时";
        break;
    default:
        text = "串口错误: " + m_port.errorString();
        break;
    }
    emit errorOccurred(text);

    // 设备被拔出等致命错误: 自动进入"已关闭"状态
    if (error == QSerialPort::ResourceError) {
        m_port.close();
        emit closed();
    }
}
