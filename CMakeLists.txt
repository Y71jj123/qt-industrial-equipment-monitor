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

add_subdirectory(src)

# ---------------------------------------------------------------------------
# 单元测试：ctest 驱动。BUILD_TESTING 由 include(CTest) 提供（默认 ON），
# 可用 -DBUILD_TESTING=OFF 关掉，出包时不必编译测试。
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING AND Qt${QT_VERSION_MAJOR}Test_FOUND)
    add_subdirectory(tests)
endif()
