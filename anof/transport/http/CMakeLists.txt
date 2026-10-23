add_library(anof_http STATIC
    DriveRequest.cc
    DriveResponse.cc
    DriveHandlers.cc
    DownloadResponse.cc
    DownloadStream.cc
    TransferRequest.cc
    TransferResponse.cc
    UploadStream.cc
    BearerCredentials.cc
    CreateDirectoryJson.cc
    JsonResponse.cc
    IdentityStream.cc
    AccountResponse.cc
    AccountRequest.cc
    IdentityRequest.cc
    IdentityResponse.cc
    Pagination.cc
    internal/IdentityJson.cc
    internal/FlatJson.cc
    RequestTrace.cc
    HttpApi.cc
    CreateDirectoryHandler.cc
)
target_link_libraries(anof_http PUBLIC anof_application muduox_http PRIVATE muduox_router muduox_middleware nlohmann_json::nlohmann_json)
target_compile_options(anof_http PRIVATE -Wall -Wextra -Wconversion -Wshadow)

if (ANOF_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
