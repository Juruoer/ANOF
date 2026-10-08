cmake_minimum_required(VERSION 3.28.3)
project(ANOF LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ANOF_BUILD_TESTS "Build bounded ANOF tests" ON)

# 基础调度与实例装配只使用 muduox 公开 API，第三方测试默认关闭。
find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Sodium REQUIRED IMPORTED_TARGET libsodium)
find_package(Threads REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
option(MUDUOX_BUILD_TESTS "Build vendored muduox tests" OFF)
option(MUDUOX_BUILD_EXAMPLES "Build vendored muduox examples" OFF)
add_subdirectory(third_party/muduox EXCLUDE_FROM_ALL)

if (ANOF_BUILD_TESTS)
    enable_testing()
    find_package(Boost 1.74 CONFIG REQUIRED COMPONENTS unit_test_framework)
    include(cmake/AnofTests.cmake)
endif ()

add_subdirectory(anof)
add_subdirectory(apps/anof)

if (ANOF_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
