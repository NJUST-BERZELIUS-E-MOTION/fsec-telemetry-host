# =====================================================================
# SerialJsonHost - LoRa 遥测上位机 (串口 JSON 解析显示工具)
#   技术栈: Qt 6 (Widgets + SerialPort) / C++17 / CMake
#   用途  : 接收 STM32 LoRa 测试板 USART1 输出的混流(JSON遥测 + 文本日志),
#           按行切分 -> 尝试解析 JSON -> 分发给数据处理出口(ISink)。
#   开发  : VSCode + CMake (MSVC2022 / MinGW 双工具链, 见 CMakePresets.json)
# =====================================================================
cmake_minimum_required(VERSION 3.25)

project(SerialJsonHost
    VERSION 0.1.0
    DESCRIPTION "LoRa serial JSON telemetry host"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------
# Qt6 定位: 由 CMakePresets.json 里的 CMAKE_PREFIX_PATH 指定
#   D:/Qt6.11/6.11.1/msvc2022_64  或  D:/Qt6.11/6.11.1/mingw_64
# ---------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets SerialPort Charts)

# 统一 Qt6 工程的推荐默认值(AUTOMOC/AUTORCC/编译选项等)
qt_standard_project_setup()

qt_add_executable(SerialJsonHost
    # ---- 程序入口 ----
    src/main.cpp
    # ---- 核心流水线 (core) ----
    src/core/Frame.h
    src/core/ISink.h
    src/core/SinkManager.h
    src/core/SinkManager.cpp
    src/core/SerialReceiver.h
    src/core/SerialReceiver.cpp
    src/core/JsonFrameParser.h
    src/core/JsonFrameParser.cpp
    src/core/TelemetryRecord.h
    src/core/TelemetryRecord.cpp
    src/core/IPostProcessor.h
    src/core/TelemetrySchema.h
    src/core/TelemetrySchema.cpp
    # ---- 数据处理出口 (sinks, 扩展点) ----
    src/sinks/FileSink.h
    src/sinks/FileSink.cpp
    src/sinks/RawLogSink.h
    src/sinks/RawLogSink.cpp
    src/sinks/TelemetrySink.h
    src/sinks/TelemetrySink.cpp
    # ---- 界面 (ui) ----
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/TelemetryTableModel.h
    src/ui/TelemetryTableModel.cpp
)

# GUI 程序: Windows 下不弹出控制台黑窗
set_target_properties(SerialJsonHost PROPERTIES WIN32_EXECUTABLE TRUE)

# 头文件根: 代码内以 #include "core/xxx.h" / "sinks/xxx.h" / "ui/xxx.h" 引用
target_include_directories(SerialJsonHost PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(SerialJsonHost PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::SerialPort
    Qt6::Charts)

# ---------------------------------------------------------------
# 源码统一按 UTF-8 处理
#  - MSVC 默认把无 BOM 源码按本地代码页(GBK)解读, 必须显式 /utf-8
#  - GCC/Clang 默认按 UTF-8 解读源码, 无需额外参数
# ---------------------------------------------------------------
if(MSVC)
    target_compile_options(SerialJsonHost PRIVATE /utf-8)
endif()

# Windows 下告警级别(可选, 便于开发期发现问题)
if(MSVC)
    target_compile_options(SerialJsonHost PRIVATE /W3)
else()
    target_compile_options(SerialJsonHost PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------
# 把 config/fields.json (遥测字段显示配置) 拷贝到可执行文件旁,
# 保证运行时一定能从 QCoreApplication::applicationDirPath()/config/ 找到
# ---------------------------------------------------------------
add_custom_command(TARGET SerialJsonHost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/config/fields.json
            $<TARGET_FILE_DIR:SerialJsonHost>/config/fields.json
    COMMENT "Copying config/fields.json next to executable"
    VERBATIM)
