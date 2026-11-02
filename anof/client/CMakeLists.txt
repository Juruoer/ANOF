add_library(anof_client STATIC
        ClientTypes.cc
        CurlHttpTransport.cc
        ProfileStore.cc
        Protocol.cc
        DriveClient.cc
        DriveProtocol.cc
        ApiClient.cc
        TransferProtocol.cc
        TransferClient.cc
        internal/TransferFiles.cc
        internal/Json.cc
)
target_include_directories(anof_client PUBLIC ${PROJECT_SOURCE_DIR})
target_compile_features(anof_client PUBLIC cxx_std_17)
target_compile_options(anof_client PRIVATE -Wall -Wextra -Wconversion -Wshadow)
target_link_libraries(anof_client PUBLIC anof_domain nlohmann_json::nlohmann_json PRIVATE CURL::libcurl PkgConfig::Sodium)

if (ANOF_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
