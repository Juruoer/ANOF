# 普通构建不下载依赖；版本门槛不是可被用户降低的cache选项。
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "ANOF currently validates native Linux builds only; cross compilation is not supported")
endif()
set(ANOF_MIN_SQLITE 3.54.1)
set(ANOF_MIN_SQLITE_NUMBER 3054001)
set(ANOF_MIN_CURL 8.5.0)
set(ANOF_MIN_CURL_NUMBER 0x080500)
# SHARED属性避免无SONAME的SQLite将构建机绝对路径写入DT_NEEDED。
add_library(SQLite::SQLite3 SHARED IMPORTED)
find_package(SQLite3 ${ANOF_MIN_SQLITE} REQUIRED)
if(SQLite3_VERSION VERSION_GREATER_EQUAL 4 OR NOT SQLite3_LIBRARY MATCHES "\\.so(\\.[0-9.]+)?$")
    message(FATAL_ERROR "ANOF requires a SQLite 3 shared library; choose a matching include/library prefix")
endif()
set_target_properties(SQLite::SQLite3 PROPERTIES IMPORTED_LOCATION "${SQLite3_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIRS}" IMPORTED_NO_SONAME TRUE)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Sodium REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3.11.3 CONFIG REQUIRED)
find_package(tomlplusplus 3.4.0 CONFIG REQUIRED)
find_package(CLI11 2.4.1 CONFIG REQUIRED)
find_package(CURL ${ANOF_MIN_CURL} REQUIRED)
configure_file("${PROJECT_SOURCE_DIR}/cmake/DependencyPolicy.h.in"
    "${PROJECT_BINARY_DIR}/generated/anof/build/DependencyPolicy.h" @ONLY)
# NO_CACHE确保依赖路径/运行库改变时不会沿用上次成功结果。
try_run(anof_dependency_run anof_dependency_compile
    SOURCES "${PROJECT_SOURCE_DIR}/cmake/CheckDependencies.cc" "${PROJECT_SOURCE_DIR}/apps/anof/RuntimeDependencies.cc"
    NO_CACHE
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR};${PROJECT_BINARY_DIR}/generated"
    LINK_LIBRARIES SQLite::SQLite3 CURL::libcurl
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    COMPILE_OUTPUT_VARIABLE anof_dependency_build_log
    RUN_OUTPUT_VARIABLE anof_dependency_run_log)
if(NOT anof_dependency_compile OR NOT "${anof_dependency_run}" STREQUAL "0")
    message(FATAL_ERROR "Dependency compile/runtime validation failed. Check headers, library paths and HTTPS support.\n${anof_dependency_build_log}\n${anof_dependency_run_log}")
endif()
message(STATUS "${anof_dependency_run_log}")
message(STATUS "SQLite include=${SQLite3_INCLUDE_DIRS}; library=${SQLite3_LIBRARY}")
message(STATUS "Dependencies: sodium=${Sodium_VERSION}; JSON=${nlohmann_json_VERSION}; toml++=${tomlplusplus_VERSION}; CLI11=${CLI11_VERSION}; curl=${CURL_VERSION_STRING}")
if(ANOF_BUILD_TESTS)
    find_package(Boost 1.74 CONFIG REQUIRED COMPONENTS unit_test_framework)
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
endif()
