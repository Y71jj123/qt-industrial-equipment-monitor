cmake_minimum_required(VERSION 3.16)

project(qt-industrial-equipment-monitor
    VERSION 0.1.0
    DESCRIPTION "Qt/C++ desktop platform for remote monitoring and management of industrial equipment"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt meta-object / resource / ui compilers
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Qt: prefer Qt6, fall back to Qt5
# ---------------------------------------------------------------------------
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Sql Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Sql Network)

# Qt Charts is optional: the trend-chart page is not wired up yet.
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Charts QUIET)

# Qt Test is optional too: 没有它只是不能跑单元测试，不影响出包。
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Test QUIET)

# Qt SerialPort is optional: Modbus RTU 走串口需要它。没装时整个 RTU 连接类
# （comm/modbusrtuconnection.*）因 QT_CONFIG(serialport) 守卫不进入编译，
# 主程序照常构建，只是不会出现 "Modbus RTU" 这个协议选项。
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS SerialPort QUIET)

message(STATUS "Qt version   : ${QT_VERSION_MAJOR} (${Qt${QT_VERSION_MAJOR}_VERSION})")
if(Qt${QT_VERSION_MAJOR}Charts_FOUND)
    message(STATUS "Qt Charts    : found")
else()
    message(STATUS "Qt Charts    : NOT found (trend chart page will be disabled)")
endif()
if(Qt${QT_VERSION_MAJOR}Test_FOUND)
    message(STATUS "Qt Test      : found (unit tests enabled)")
else()
    message(STATUS "Qt Test      : NOT found (unit tests disabled)")
endif()
if(Qt${QT_VERSION_MAJOR}SerialPort_FOUND)
    message(STATUS "Qt SerialPort: found (Modbus RTU enabled)")
else()
    message(STATUS "Qt SerialPort: NOT found (Modbus RTU disabled)")
endif()

# 源码根目录：协议插件（plugins/）与性能夹具（tools/bench）都要用它做 include 路径。
# 定义在顶层是为了让它们都能拿到，且不必各自去拼相对路径。
set(MONITOR_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_subdirectory(src)

# ---------------------------------------------------------------------------
# 单元测试：ctest 驱动。BUILD_TESTING 由 include(CTest) 提供（默认 ON），
# 可用 -DBUILD_TESTING=OFF 关掉，出包时不必编译测试。
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING AND Qt${QT_VERSION_MAJOR}Test_FOUND)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# 性能基线夹具：默认关闭（-DMONITOR_BUILD_BENCH=ON 打开）。
# 它只在"想复现 README 里的数字"时才需要，让 CI 每次构建都编它是纯浪费。
# ---------------------------------------------------------------------------
option(MONITOR_BUILD_BENCH "构建性能基线夹具 monitor_bench（默认关闭）" OFF)
if(MONITOR_BUILD_BENCH)
    message(STATUS "Bench        : enabled (monitor_bench)")
    add_subdirectory(tools/bench)
endif()

# ---------------------------------------------------------------------------
# 安装与打包（CPack）
#
# 目标是产出一个**解压即用**的压缩包：里面不仅有 exe，还有 Qt 运行时依赖。
# 少了依赖的包在现场就是"双击报找不到 Qt6Core.dll"，等于没打。
# ---------------------------------------------------------------------------
install(FILES README.md LICENSE DESTINATION . OPTIONAL)

include(CPack)
set(CPACK_PACKAGE_NAME "qt-industrial-equipment-monitor")
set(CPACK_PACKAGE_VENDOR "Y71jj123")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "工业设备远程监控管理平台桌面客户端")
# 文件名用纯 ASCII，避免各种解压工具在中文名上翻车
if(WIN32)
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-win64")
else()
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-linux-x64")
endif()
# 默认 ZIP：不依赖任何额外工具链。
# 想做成点下一步就能装的安装器，在配置时加 -DCPACK_GENERATOR=NSIS 即可（需本机有 makensis）。
set(CPACK_GENERATOR "ZIP")
