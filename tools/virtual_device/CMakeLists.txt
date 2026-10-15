# ---------------------------------------------------------------------------
# 虚拟设备：可独立运行的 Modbus TCP 从站模拟器（演示 / 联调用）。
#
# 它**刻意不链接 monitor_core** —— 工具就该独立：不会因为主程序改接口而被动跟改，
# 也不会把 UI、数据库这些无关依赖带进来。代价是点位表要手工与主程序
# defaultTagPoints() 对齐，这个约定写在 signalgenerator.h 的注释里。
# ---------------------------------------------------------------------------
set(VIRTUAL_DEVICE_SOURCES
    main.cpp
    virtualdevicewindow.h
    virtualdevicewindow.cpp
    modbustcpslave.h
    modbustcpslave.cpp
    signalgenerator.h
    signalgenerator.cpp
)

add_executable(virtual_device ${VIRTUAL_DEVICE_SOURCES})

target_include_directories(virtual_device PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(virtual_device PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)

# 产物落到 build/bin/，与主程序分开放，演示时一眼能找到。
set_target_properties(virtual_device PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# 与主程序同一套标准：零警告。
if(MSVC)
    target_compile_options(virtual_device PRIVATE /utf-8 /W4)
else()
    target_compile_options(virtual_device PRIVATE -Wall -Wextra)
endif()
