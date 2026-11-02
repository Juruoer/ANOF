anof_add_unit_test(anof_http_transfer_protocol_unittest TransferProtocol_unittest.cc)
set_tests_properties(anof_http_transfer_protocol_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_transfer_protocol_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_transfer_protocol_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
anof_add_unit_test(anof_http_upload_handlers_unittest UploadHandlers_unittest.cc)
set_tests_properties(anof_http_upload_handlers_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_upload_handlers_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_upload_handlers_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_account_writes_unittest AccountWrites_unittest.cc)
set_tests_properties(anof_http_account_writes_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_account_writes_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_account_writes_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_account_reads_unittest AccountReads_unittest.cc)
set_tests_properties(anof_http_account_reads_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_account_reads_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_account_reads_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_account_protocol_unittest AccountProtocol_unittest.cc)
set_tests_properties(anof_http_account_protocol_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_account_protocol_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_account_protocol_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_identity_handlers_unittest IdentityHandlers_unittest.cc)
set_tests_properties(anof_http_identity_handlers_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_identity_handlers_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_identity_handlers_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_reads_unittest ReadHandlers_unittest.cc)
set_tests_properties(anof_http_reads_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_reads_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_reads_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_identity_stream_unittest IdentityStream_unittest.cc)
set_tests_properties(anof_http_identity_stream_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_identity_stream_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_identity_stream_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_identity_protocol_unittest IdentityProtocol_unittest.cc)
set_tests_properties(anof_http_identity_protocol_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_identity_protocol_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_identity_protocol_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_protocol_unittest Protocol_unittest.cc)
set_tests_properties(anof_http_protocol_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_protocol_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_protocol_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_api_unittest HttpApi_unittest.cc)
set_tests_properties(anof_http_api_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_api_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_api_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_create_directory_unittest CreateDirectoryHandler_unittest.cc)
set_tests_properties(anof_http_create_directory_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_create_directory_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_create_directory_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

# 每个公开头独立编译，避免调用方依赖偶然的 include 顺序。
set(http_public_headers
        DriveRequest.h
        DriveResponse.h
        DriveHandlers.h
        DownloadResponse.h
        DownloadStream.h
        TransferRequest.h
        TransferResponse.h
        UploadStream.h
        BrowserSecurity.h
        RequestCredentials.h
        CreateDirectoryJson.h
        JsonResponse.h
        IdentityStream.h
        AccountResponse.h
        AccountRequest.h
        IdentityRequest.h
        IdentityResponse.h
        Pagination.h
        RequestTrace.h
        HttpApi.h
        CreateDirectoryHandler.h
)
set(http_header_sources)
foreach (header IN LISTS http_public_headers)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cc")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <anof/transport/http/${header}>\n")
    list(APPEND http_header_sources "${source}")
endforeach ()
add_library(anof_http_public_headers_check OBJECT ${http_header_sources})
target_link_libraries(anof_http_public_headers_check PRIVATE anof_http)
target_compile_options(anof_http_public_headers_check PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_download_protocol_unittest DownloadProtocol_unittest.cc)
set_tests_properties(anof_http_download_protocol_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_download_protocol_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_download_protocol_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
anof_add_unit_test(anof_http_download_handlers_unittest DownloadHandlers_unittest.cc)
set_tests_properties(anof_http_download_handlers_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_download_handlers_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_download_handlers_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_drive_protocol_unittest DriveProtocol_unittest.cc)
set_tests_properties(anof_http_drive_protocol_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_drive_protocol_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_drive_protocol_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
anof_add_unit_test(anof_http_drive_handlers_unittest DriveHandlers_unittest.cc)
set_tests_properties(anof_http_drive_handlers_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http")
target_link_libraries(anof_http_drive_handlers_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_drive_handlers_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_browser_security_unittest BrowserSecurity_unittest.cc)
set_tests_properties(anof_http_browser_security_unittest PROPERTIES TIMEOUT 30 LABELS "unit;http;security")
target_link_libraries(anof_http_browser_security_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_browser_security_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
