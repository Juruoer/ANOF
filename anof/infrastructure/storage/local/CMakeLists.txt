target_sources(anof_infrastructure PRIVATE
        LocalMaintenanceFileStore.cc
        internal/BackupEncoding.cc LocalTransferFileStore.cc)
if (ANOF_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
