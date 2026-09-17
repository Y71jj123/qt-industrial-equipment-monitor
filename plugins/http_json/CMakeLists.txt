# HTTP / JSON 协议插件 —— 外部插件的示例与样板。
#
# 刻意做成 MODULE 动态库而不是编进主程序：这是"协议真的可插拔"的唯一硬证据。
# 它就是"新增一个协议要写多少东西"的答案 —— 一个头文件、一个 cpp、四行 CMake。

add_library(monitor_protocol_http_json MODULE
    httpjsonplugin.h
    httpjsonplugin.cpp
)

target_include_directories(monitor_protocol_http_json PRIVATE ${MONITOR_SRC_DIR})

# monitor_core 是静态库，会被链进这个插件里 —— 插件因此拿到了 DeviceConnection、
# DeviceInfo、Log 这些公共设施。注意 Log::instance() 是按 QCoreApplication
# 下的对象查的（不是函数内静态），所以插件打出来的日志会和主程序**共用同一个**
# 日志文件与滚动计数，不会出现两套实例互相打架。
target_link_libraries(monitor_protocol_http_json PRIVATE
    monitor_core
    Qt${QT_VERSION_MAJOR}::Network
)

if(MSVC)
    target_compile_options(monitor_protocol_http_json PRIVATE /utf-8 /W4)
else()
    target_compile_options(monitor_protocol_http_json PRIVATE -Wall -Wextra)
endif()

# 产物直接落到可执行文件旁的 plugins/protocols/：编完即可被加载，
# 少一个"忘了拷插件"的坑（这个坑在 CI 上尤其难查）。
set_target_properties(monitor_protocol_http_json PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${MONITOR_PROTOCOL_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${MONITOR_PROTOCOL_DIR}"
)

# 安装时放进 <prefix>/plugins/protocols/ —— 主程序会同时找
# <exe>/plugins/protocols 与 <exe>/../plugins/protocols 两处，两个布局都认得。
install(TARGETS monitor_protocol_http_json
        RUNTIME DESTINATION plugins/protocols
        LIBRARY DESTINATION plugins/protocols)
