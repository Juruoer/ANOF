anof_add_unit_test(anof_http_protocol_unittest Protocol_unittest.cc)
target_link_libraries(anof_http_protocol_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_protocol_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_api_unittest HttpApi_unittest.cc)
target_link_libraries(anof_http_api_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_api_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_http_create_directory_unittest CreateDirectoryHandler_unittest.cc)
target_link_libraries(anof_http_create_directory_unittest PRIVATE anof_http nlohmann_json::nlohmann_json)
target_compile_options(anof_http_create_directory_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

# 每个公开头独立编译，避免调用方依赖偶然的 include 顺序。
set(http_public_headers
    BearerCredentials.h
    CreateDirectoryJson.h
    JsonResponse.h
    RequestTrace.h
    HttpApi.h
    CreateDirectoryHandler.h
)
set(http_header_sources)
foreach(header IN LISTS http_public_headers)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cc")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <anof/transport/http/${header}>\n")
    list(APPEND http_header_sources "${source}")
endforeach()
add_library(anof_http_public_headers_check OBJECT ${http_header_sources})
target_link_libraries(anof_http_public_headers_check PRIVATE anof_http)
target_compile_options(anof_http_public_headers_check PRIVATE -Wall -Wextra -Wconversion -Wshadow)
