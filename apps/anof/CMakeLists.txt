add_library(anof_process_support STATIC ShutdownSignals.cc)
target_link_libraries(anof_process_support PUBLIC anof_application Threads::Threads)
target_compile_options(anof_process_support PRIVATE -Wall -Wextra -Wconversion -Wshadow)

if (ANOF_BUILD_TESTS)
    anof_add_unit_test(anof_shutdown_signals_unittest tests/ShutdownSignals_unittest.cc)
    target_link_libraries(anof_shutdown_signals_unittest PRIVATE anof_process_support)
    target_compile_options(anof_shutdown_signals_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
endif ()

add_executable(anof Main.cc Serve.cc)
target_link_libraries(anof PRIVATE
    anof_process_support anof_instance_runtime anof_infrastructure CLI11::CLI11
)
target_compile_options(anof PRIVATE -Wall -Wextra -Wconversion -Wshadow)
