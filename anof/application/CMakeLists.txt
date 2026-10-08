add_library(anof_application STATIC
    drive/CreateDirectory.cc
    maintenance/PrepareInstance.cc
)
target_link_libraries(anof_application PUBLIC anof_domain)
target_compile_options(anof_application PRIVATE -Wall -Wextra -Wconversion -Wshadow)

if (ANOF_BUILD_TESTS)
    add_subdirectory(maintenance/tests)
endif ()
