# 单元测试。父工程已确认 Qt Test 可用才会 add_subdirectory(tests)。

# Windows 上 Qt 的 DLL 不在系统 PATH 里，测试进程必须自己把 Qt 的 bin 目录带上，
# 否则 ctest 会以 0xc0000135（找不到 DLL）整体失败 —— 而人肉双击又是好的，
# 这种假失败最容易被误判成"测试代码写错了"。
if(WIN32)
    get_target_property(_qt_qmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
    get_filename_component(MONITOR_QT_BIN_DIR "${_qt_qmake}" DIRECTORY)
    message(STATUS "Qt bin dir   : ${MONITOR_QT_BIN_DIR}")
endif()

# 统一的测试目标创建：链接 monitor_core = **产品实际运行的那份代码**，
# 而不是把源码再编译一遍（否则测试守的是一份影子实现）。
function(monitor_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE monitor_core Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME ${name} COMMAND ${name})

    # 测试全程不碰屏幕：用 QTEST_GUILESS_MAIN + offscreen 双保险，
    # CI 上没有显示器也能跑。
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        # 用 path_list_prepend 而不是手拼 PATH：PATH 分隔符在 Windows 上是 ';'，
        # 手拼会被 CMake 当成列表分隔符切碎。
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${MONITOR_QT_BIN_DIR}")
    endif()
endfunction()

monitor_add_test(tst_alarmengine)
monitor_add_test(tst_datastorage)
monitor_add_test(tst_configio)
