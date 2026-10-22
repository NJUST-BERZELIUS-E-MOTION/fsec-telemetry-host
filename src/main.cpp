#include <QApplication>

#include "ui/MainWindow.h"

// =====================================================================
// SerialJsonHost 程序入口
//   LoRa 遥测上位机 —— 解析并显示嵌入式串口输出的 JSON 数据
//   数据流: 串口 -> 按行切分 -> 逐行解析 JSON -> ISink 出口分发
// =====================================================================
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 用于 QSettings 记住上次使用的端口/波特率
    QApplication::setOrganizationName("FSEC-2026");
    QApplication::setApplicationName("SerialJsonHost");

    MainWindow window;
    window.show();
    return app.exec();
}
