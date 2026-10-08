add_library(anof_http STATIC
    BearerCredentials.cc
    CreateDirectoryJson.cc
    JsonResponse.cc
    RequestTrace.cc
    HttpApi.cc
    CreateDirectoryHandler.cc
)
target_link_libraries(anof_http PUBLIC anof_application muduox_http PRIVATE muduox_router muduox_middleware nlohmann_json::nlohmann_json)
target_compile_options(anof_http PRIVATE -Wall -Wextra -Wconversion -Wshadow)

if (ANOF_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
