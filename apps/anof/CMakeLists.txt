add_library(anof_maintenance_output STATIC MaintenanceOutput.cc MaintenancePlanJson.cc)
target_link_libraries(anof_maintenance_output PUBLIC anof_application nlohmann_json::nlohmann_json)
target_compile_options(anof_maintenance_output PRIVATE -Wall -Wextra -Wconversion -Wshadow)

add_library(anof_process_support STATIC ShutdownSignals.cc)
target_link_libraries(anof_process_support PUBLIC anof_application Threads::Threads)
target_compile_options(anof_process_support PRIVATE -Wall -Wextra -Wconversion -Wshadow)

if (ANOF_BUILD_TESTS)
    anof_add_unit_test(anof_maintenance_output_unittest tests/MaintenanceOutput_unittest.cc)
    set_tests_properties(anof_maintenance_output_unittest PROPERTIES TIMEOUT 30 LABELS "unit;cli")
    target_link_libraries(anof_maintenance_output_unittest PRIVATE anof_maintenance_output)
    anof_add_unit_test(anof_shutdown_signals_unittest tests/ShutdownSignals_unittest.cc)
    set_tests_properties(anof_shutdown_signals_unittest PROPERTIES TIMEOUT 30 LABELS "integration;process")
    target_link_libraries(anof_shutdown_signals_unittest PRIVATE anof_process_support)
    target_compile_options(anof_shutdown_signals_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
endif ()

add_library(anof_terminal_support STATIC TerminalPassword.cc)
target_link_libraries(anof_terminal_support PUBLIC anof_domain PRIVATE Threads::Threads)
target_compile_options(anof_terminal_support PRIVATE -Wall -Wextra -Wconversion -Wshadow)

add_library(anof_client_commands STATIC ClientCommands.cc
        TransferCommands.cc DriveCommands.cc ClientOutput.cc)
target_link_libraries(anof_client_commands PUBLIC anof_client PRIVATE anof_terminal_support CLI11::CLI11 nlohmann_json::nlohmann_json)
target_compile_options(anof_client_commands PRIVATE -Wall -Wextra -Wconversion -Wshadow)

add_library(anof_init_options STATIC InitOptions.cc)
target_link_libraries(anof_init_options PUBLIC anof_application)
target_compile_options(anof_init_options PRIVATE -Wall -Wextra -Wconversion -Wshadow)

add_library(anof_init_support STATIC Init.cc)
target_link_libraries(anof_init_support PUBLIC anof_init_options anof_terminal_support PRIVATE anof_instance_setup Threads::Threads)
target_compile_options(anof_init_support PRIVATE -Wall -Wextra -Wconversion -Wshadow)

add_library(anof_offline_commands STATIC OfflineCommands.cc)
target_link_libraries(anof_offline_commands PUBLIC anof_maintenance_output PRIVATE anof_offline_maintenance anof_infrastructure anof_terminal_support anof_process_support CLI11::CLI11)
target_compile_options(anof_offline_commands PRIVATE -Wall -Wextra -Wconversion -Wshadow)

add_executable(anof Main.cc Serve.cc RuntimeDependencies.cc)
target_include_directories(anof PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(anof PRIVATE SQLite::SQLite3 CURL::libcurl)
target_link_libraries(anof PRIVATE
        anof_offline_commands anof_process_support anof_init_support anof_client_commands anof_instance_runtime anof_infrastructure CLI11::CLI11
)
target_compile_options(anof PRIVATE -Wall -Wextra -Wconversion -Wshadow)

if (ANOF_BUILD_TESTS)
    anof_add_unit_test(anof_init_command_unittest tests/Init_unittest.cc)
    set_tests_properties(anof_init_command_unittest PROPERTIES TIMEOUT 30 LABELS "unit;cli")
    target_link_libraries(anof_init_command_unittest PRIVATE anof_init_options)
endif ()
if (ANOF_BUILD_TESTS)
    anof_add_unit_test(anof_terminal_password_unittest tests/TerminalPassword_unittest.cc)
    set_tests_properties(anof_terminal_password_unittest PROPERTIES TIMEOUT 30 LABELS "integration;cli;process;security")
    target_link_libraries(anof_terminal_password_unittest PRIVATE anof_init_support)
endif ()
