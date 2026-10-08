anof_add_unit_test(anof_runtime_unittest Runtime_unittest.cc)
target_link_libraries(anof_runtime_unittest PRIVATE anof_runtime)
target_compile_options(anof_runtime_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
