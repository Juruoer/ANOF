anof_add_unit_test(anof_instance_configuration_unittest InstanceConfiguration_unittest.cc)
set_tests_properties(anof_instance_configuration_unittest PROPERTIES TIMEOUT 30 LABELS "integration;config")
target_link_libraries(anof_instance_configuration_unittest PRIVATE anof_infrastructure)
target_compile_options(anof_instance_configuration_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

# 保留仅链接配置、路径和应用端口的独立依赖检查，与完整 infrastructure 链接分别验收。
anof_add_unit_test(anof_m5_configuration_unittest InstanceConfiguration_unittest.cc)
set_tests_properties(anof_m5_configuration_unittest PROPERTIES TIMEOUT 30 LABELS "integration;config")
target_sources(anof_m5_configuration_unittest PRIVATE
        ../TomlInstanceConfiguration.cc
        ../internal/ConfigurationText.cc
        ../../system/LinuxInstanceLock.cc
        ../../system/internal/InstanceFiles.cc
        ../../system/internal/BoundedFileRead.cc
)
target_link_libraries(anof_m5_configuration_unittest PRIVATE anof_application tomlplusplus::tomlplusplus)
target_compile_options(anof_m5_configuration_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
