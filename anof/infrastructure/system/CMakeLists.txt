target_sources(anof_infrastructure PRIVATE
    SystemClock.h
    LinuxInstanceLock.h
    LinuxInstanceLock.cc
    internal/BoundedFileRead.h
    internal/BoundedFileRead.cc
    internal/InstanceFiles.h
    internal/InstanceFiles.cc
    SystemClock.cc
    internal/UtcTimeConversion.h
    internal/UtcTimeConversion.cc
)

if (ANOF_BUILD_TESTS)
    set(system_headers SystemClock.h LinuxInstanceLock.h)
    set(system_header_sources)
    foreach(header IN LISTS system_headers)
        set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cc")
        file(GENERATE OUTPUT "${source}" CONTENT "#include <anof/infrastructure/system/${header}>\n")
        list(APPEND system_header_sources "${source}")
    endforeach()
    add_library(anof_system_headers_check OBJECT ${system_header_sources})
    target_link_libraries(anof_system_headers_check PRIVATE anof_application)
    add_subdirectory(tests)
endif ()
