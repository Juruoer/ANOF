add_library(anof_runtime STATIC
    AuthenticationLimiter.cc
    IdentityExecutor.cc
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

# 离线初始化装配独立于 HTTP；由 CLI 和真实实例测试复用。
add_library(anof_instance_setup STATIC InstanceInitialization.cc)
target_link_libraries(anof_instance_setup PUBLIC anof_application PRIVATE anof_infrastructure)
target_compile_options(anof_instance_setup PRIVATE -Wall -Wextra -Wconversion -Wshadow)

add_library(anof_identity_resources STATIC InstanceIdentity.cc)
target_link_libraries(anof_identity_resources PUBLIC anof_runtime PRIVATE anof_infrastructure)
target_compile_options(anof_identity_resources PRIVATE -Wall -Wextra -Wconversion -Wshadow)
target_link_libraries(anof_instance_runtime PRIVATE anof_identity_resources)
