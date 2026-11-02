find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)

add_library(anof_test_old_sqlite SHARED RuntimeOverride.cc)
target_compile_definitions(anof_test_old_sqlite PRIVATE ANOF_TEST_OLD_SQLITE)
add_library(anof_test_bad_curl SHARED RuntimeOverride.cc)
target_link_libraries(anof_test_bad_curl PRIVATE CURL::libcurl)
add_test(NAME anof_runtime_dependencies_test COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime_guard.py" "$<TARGET_FILE:anof>"
    "$<TARGET_FILE:anof_test_old_sqlite>" "$<TARGET_FILE:anof_test_bad_curl>" "${ANOF_VERSION}")
set_tests_properties(anof_runtime_dependencies_test PROPERTIES TIMEOUT 30 LABELS "integration;build;process")
