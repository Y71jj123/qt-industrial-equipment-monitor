set(PROJECT_SOURCES
    main.cpp

    # UI layer
    ui/theme.h
    ui/theme.cpp
    ui/mainwindow.h
    ui/mainwindow.cpp
    ui/alarmpanel.h
    ui/alarmpanel.cpp
    ui/alarmhistorypanel.h
    ui/alarmhistorypanel.cpp
    ui/alarmnotifier.h
    ui/alarmnotifier.cpp
    ui/overviewpanel.h
    ui/overviewpanel.cpp
    ui/devicedetailpanel.h
    ui/devicedetailpanel.cpp
    ui/devicedialog.h
    ui/devicedialog.cpp
    ui/historypanel.h
    ui/historypanel.cpp
    ui/rulepanel.h
    ui/rulepanel.cpp
    ui/controlpanel.h
    ui/controlpanel.cpp
    ui/reportpanel.h
    ui/reportpanel.cpp
    ui/logindialog.h
    ui/logindialog.cpp
    ui/advancedchartview.h
    ui/advancedchartview.cpp

    # Communication layer (protocol abstraction)
    comm/deviceconnection.h
    comm/mockconnection.h
    comm/mockconnection.cpp
    comm/modbusconnection.h
    comm/modbusconnection.cpp
    comm/mqttconnection.h
    comm/mqttconnection.cpp

    # Core layer
    core/devicemanager.h
    core/devicemanager.cpp
    core/acquisitionscheduler.h
    core/acquisitionscheduler.cpp
    core/alarmengine.h
    core/alarmengine.cpp

    # Storage layer
    storage/datastorage.h
    storage/datastorage.cpp

    # Utils
    utils/logger.h
    utils/logger.cpp
    utils/configio.h
    utils/configio.cpp
    utils/excelexporter.h
    utils/excelexporter.cpp
)

add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

# Allow "core/xxx.h" style includes from within src/
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::Network
)

if(Qt${QT_VERSION_MAJOR}Charts_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt${QT_VERSION_MAJOR}::Charts)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_QT_CHARTS)
endif()

# 应用名与版本号（从顶层 project() 继承），供源码中的 APP_NAME / APP_VERSION 使用
target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_NAME="${PROJECT_NAME}"
    APP_VERSION="${PROJECT_VERSION}"
)

# MSVC needs this to read UTF-8 source files correctly (Chinese literals).
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

# Windows: no console window in release builds
if(WIN32 AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
