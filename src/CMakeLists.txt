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
    utils/crashhandler.h
    utils/crashhandler.cpp
    utils/configio.h
    utils/configio.cpp
    utils/excelexporter.h
    utils/excelexporter.cpp
)

# ---------------------------------------------------------------------------
# 核心库：除 main.cpp 外的全部源码。
#
# 拆成库不只是为了整洁 —— 单元测试要能直接链接它。否则测试只能把源码再编译一遍，
# 于是"测试编译的代码"和"程序实际跑的代码"会变成两份，测试的保证也就打了折。
# ---------------------------------------------------------------------------
list(REMOVE_ITEM PROJECT_SOURCES main.cpp)

add_library(monitor_core STATIC ${PROJECT_SOURCES})

# Allow "core/xxx.h" style includes from within src/
# PUBLIC：链接本库的目标（主程序 / 测试）都要能用同样的 include 形式
target_include_directories(monitor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(monitor_core PUBLIC
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::Network
)

if(Qt${QT_VERSION_MAJOR}Charts_FOUND)
    target_link_libraries(monitor_core PUBLIC Qt${QT_VERSION_MAJOR}::Charts)
    target_compile_definitions(monitor_core PUBLIC HAVE_QT_CHARTS)
endif()

if(WIN32)
    # 崩溃转储用到 MiniDumpWriteDump
    target_link_libraries(monitor_core PUBLIC dbghelp)
endif()

# 应用名与版本号（从顶层 project() 继承），供源码中的 APP_NAME / APP_VERSION 使用
target_compile_definitions(monitor_core PUBLIC
    APP_NAME="${PROJECT_NAME}"
    APP_VERSION="${PROJECT_VERSION}"
)

# 警告开关用 PUBLIC 传出去：测试代码也按"零警告"标准编译，
# 否则很容易出现"产品代码干净、测试代码一堆警告"的双标。
if(MSVC)
    target_compile_options(monitor_core PUBLIC /utf-8)
    target_compile_options(monitor_core PUBLIC /W4)
else()
    target_compile_options(monitor_core PUBLIC -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# 可执行文件：只剩 main.cpp，其余逻辑全部来自 monitor_core
# ---------------------------------------------------------------------------
add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE monitor_core)

# Windows: no console window in release builds
if(WIN32 AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# ---------------------------------------------------------------------------
# 安装规则：只装可执行文件，Qt 运行时依赖交给下面的部署脚本带进来
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# 让 Qt 把自己的依赖（DLL / 平台插件 / SQL 驱动）也复制进安装目录 ——
# 否则装出来的东西一运行就是"找不到 Qt6Core.dll"，等于没打这个包。
if(WIN32 OR APPLE)
    # NO_TRANSLATIONS：界面文案全是中文硬编码，带一堆 qt_*.qm 只是白占 20+ MB。
    # 这个选项需要 Qt 6.5+；更老的 Qt 上会退化成多带几个翻译文件，不影响功能。
    qt_generate_deploy_app_script(
        TARGET ${PROJECT_NAME}
        OUTPUT_SCRIPT monitor_deploy_script
        NO_TRANSLATIONS
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${monitor_deploy_script})
endif()
