anof_add_unit_test(anof_client_protocol_test Protocol_unittest.cc)
set_tests_properties(anof_client_protocol_test PROPERTIES TIMEOUT 30 LABELS "unit;client")
target_link_libraries(anof_client_protocol_test PRIVATE anof_client nlohmann_json::nlohmann_json)
anof_add_unit_test(anof_client_profile_test ProfileStore_unittest.cc)
set_tests_properties(anof_client_profile_test PROPERTIES TIMEOUT 30 LABELS "integration;client;filesystem")
target_link_libraries(anof_client_profile_test PRIVATE anof_client nlohmann_json::nlohmann_json)
anof_add_unit_test(anof_client_http_test CurlHttpTransport_unittest.cc)
set_tests_properties(anof_client_http_test PROPERTIES TIMEOUT 30 LABELS "integration;client;http;tls;process")
target_link_libraries(anof_client_http_test PRIVATE anof_client Threads::Threads)
# Python 标准库 ssl 只用于本地 TLS 故障夹具，无需额外 C++ 开发库。
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
target_compile_definitions(anof_client_http_test PRIVATE
        ANOF_TEST_PYTHON="${Python3_EXECUTABLE}"
        ANOF_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
anof_add_unit_test(anof_client_transfer_test TransferClient_unittest.cc)
set_tests_properties(anof_client_transfer_test PROPERTIES TIMEOUT 30 LABELS "integration;client;filesystem;transfer")
target_link_libraries(anof_client_transfer_test PRIVATE anof_client)

anof_add_unit_test(anof_client_drive_test DriveClient_unittest.cc)
set_tests_properties(anof_client_drive_test PROPERTIES TIMEOUT 30 LABELS "unit;client")
target_link_libraries(anof_client_drive_test PRIVATE anof_client)
