function(anof_add_unit_test target source)
    add_executable(${target} ${source})
    target_compile_definitions(${target} PRIVATE BOOST_TEST_DYN_LINK)
    target_link_libraries(${target} PRIVATE anof_domain Boost::unit_test_framework)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 30 LABELS "unit")
endfunction()

# 后端会话提供 factory_source，实现 makeTestMetadataBackend()。
# 契约源始终编译，但有真实后端工厂后才注册可执行验收。
function(anof_add_persistence_contract_test target factory_source backend_target)
    add_executable(${target} ${factory_source})
    target_link_libraries(${target} PRIVATE anof_persistence_contract_cases ${backend_target})
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 60 LABELS "persistence_contract")
endfunction()
