# 性能基线夹具。
#
# 默认**不编译**（-DMONITOR_BUILD_BENCH=ON 才编）：它是"想复现数字时跑一次"的工具，
# 让 CI 每次构建都编它纯属浪费；但它留在仓库里，README 里的数字随时可以复现与复核 ——
# 一个不能复现的性能数字等于没有。
add_executable(monitor_bench bench_main.cpp)

target_include_directories(monitor_bench PRIVATE ${MONITOR_SRC_DIR})
target_link_libraries(monitor_bench PRIVATE monitor_core)

if(MSVC)
    target_compile_options(monitor_bench PRIVATE /utf-8 /W4)
else()
    target_compile_options(monitor_bench PRIVATE -Wall -Wextra)
endif()
