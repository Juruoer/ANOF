target_sources(anof_infrastructure PRIVATE LocalTransferFileStore.cc)
if(ANOF_BUILD_TESTS)
    add_subdirectory(tests)
endif()
