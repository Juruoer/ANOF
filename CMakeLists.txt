cmake_minimum_required(VERSION 3.28.3)
project(ANOF VERSION 0.1.0 LANGUAGES CXX)
configure_file(cmake/Version.h.in generated/anof/build/Version.h @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ANOF_BUILD_TESTS "Build bounded ANOF tests" ON)
option(ANOF_ENABLE_INSTALL "Enable explicit ANOF runtime installation" OFF)
option(ANOF_BUNDLE_SQLITE "Install selected SQLite privately with relative RUNPATH" ON)

enable_testing() # 即使关闭测试也刷新生成的CTest目录，避免沿用旧测试入口。
include(cmake/Dependencies.cmake)
option(MUDUOX_BUILD_TESTS "Build vendored muduox tests" OFF)
option(MUDUOX_BUILD_EXAMPLES "Build vendored muduox examples" OFF)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/muduox/CMakeLists.txt")
    message(FATAL_ERROR "Missing muduox submodule. Run: git submodule update --init --recursive")
endif()
add_subdirectory(third_party/muduox EXCLUDE_FROM_ALL)

if (ANOF_BUILD_TESTS)
    include(cmake/AnofTests.cmake)
endif ()

add_subdirectory(anof)
add_subdirectory(apps/anof)

include(cmake/Install.cmake)

if (ANOF_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
