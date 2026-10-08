add_library(anof_runtime STATIC
    CompletionPump.cc
    MetadataExecutor.cc
    AnofRuntime.cc
)
target_link_libraries(anof_runtime PUBLIC anof_application muduox_net Threads::Threads)
target_compile_options(anof_runtime PRIVATE -Wall -Wextra -Wconversion -Wshadow)

if (ANOF_BUILD_TESTS)
    add_subdirectory(tests)
endif ()

# 具体装配单独链接适配器，保留基础调度库的依赖边界。
add_library(anof_instance_runtime STATIC InstanceServer.cc)
target_link_libraries(anof_instance_runtime PUBLIC anof_runtime anof_http PRIVATE anof_infrastructure)
target_compile_options(anof_instance_runtime PRIVATE -Wall -Wextra -Wconversion -Wshadow)
