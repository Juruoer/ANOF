add_library(anof_application STATIC
        maintenance/Backup.cc
        maintenance/Fsck.cc
        maintenance/Repair.cc
        maintenance/RepairInvariant.cc
        maintenance/MaintenanceEncoding.cc
        transfer/TransferRecovery.cc
        transfer/DownloadService.cc
        transfer/UploadService.cc
        drive/MaintenanceService.cc
        drive/PurgeTree.cc
        drive/DriveService.cc
        drive/DriveCommands.cc
        drive/CreateDirectory.cc
        identity/AccountService.cc
        identity/IdentityService.cc
        maintenance/InitializeInstance.cc
        maintenance/Initialization.cc
        maintenance/PrepareInstance.cc
)
target_link_libraries(anof_application PUBLIC anof_domain)
target_compile_options(anof_application PRIVATE -Wall -Wextra -Wconversion -Wshadow)

if (ANOF_BUILD_TESTS)
    add_subdirectory(maintenance/tests)
endif ()
