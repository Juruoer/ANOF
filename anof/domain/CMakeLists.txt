add_library(anof_domain STATIC
        maintenance/MaintenanceValues.cc
        identity/PublicOrigin.cc
        drive/DrivePolicy.cc
        drive/DirectoryPolicy.cc
        identity/IdentityValues.cc
        transfer/TransferValues.cc
        transfer/TransferPolicy.cc
        transfer/Storage.cc
        drive/Quota.cc
)
target_include_directories(anof_domain PUBLIC ${PROJECT_SOURCE_DIR})
target_compile_features(anof_domain PUBLIC cxx_std_17)
target_compile_options(anof_domain PRIVATE -Wall -Wextra -Wconversion -Wshadow)

if (ANOF_BUILD_TESTS)
    add_subdirectory(maintenance/tests)
    add_subdirectory(transfer/tests)
    add_subdirectory(drive/tests)
    add_subdirectory(identity/tests)
    add_subdirectory(../core/tests ${CMAKE_CURRENT_BINARY_DIR}/core_tests)
endif ()
