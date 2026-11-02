anof_add_unit_test(anof_session_token_codec_unittest SessionTokenCodec_unittest.cc)
set_tests_properties(anof_session_token_codec_unittest PROPERTIES TIMEOUT 15 LABELS "unit;crypto")
target_link_libraries(anof_session_token_codec_unittest PRIVATE
        anof_infrastructure PkgConfig::Sodium
)
target_compile_options(anof_session_token_codec_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_request_identity_unittest RequestIdentity_unittest.cc)
set_tests_properties(anof_request_identity_unittest PROPERTIES TIMEOUT 15 LABELS "unit;crypto")
target_link_libraries(anof_request_identity_unittest PRIVATE
        anof_infrastructure PkgConfig::Sodium
)
target_compile_options(anof_request_identity_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_node_id_generator_unittest NodeIdGenerator_unittest.cc)
set_tests_properties(anof_node_id_generator_unittest PROPERTIES TIMEOUT 15 LABELS "unit;crypto")
target_link_libraries(anof_node_id_generator_unittest PRIVATE
        anof_infrastructure
)
target_compile_options(anof_node_id_generator_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_password_hasher_unittest PasswordHasher_unittest.cc)
set_tests_properties(anof_password_hasher_unittest PROPERTIES TIMEOUT 30 LABELS "unit;crypto")
target_link_libraries(anof_password_hasher_unittest PRIVATE anof_infrastructure PkgConfig::Sodium)
target_compile_options(anof_password_hasher_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_m2_credentials_unittest M2Credentials_unittest.cc)
set_tests_properties(anof_m2_credentials_unittest PROPERTIES TIMEOUT 15 LABELS "unit;crypto")
target_link_libraries(anof_m2_credentials_unittest PRIVATE anof_infrastructure PkgConfig::Sodium)
target_compile_options(anof_m2_credentials_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

anof_add_unit_test(anof_maintenance_id_unittest MaintenanceId_unittest.cc)
set_tests_properties(anof_maintenance_id_unittest PROPERTIES TIMEOUT 15 LABELS "unit;crypto")
target_sources(anof_maintenance_id_unittest PRIVATE
        ../SodiumTransferIdGenerator.cc
        ../internal/SodiumSupport.cc
)
target_link_libraries(anof_maintenance_id_unittest PRIVATE anof_application PkgConfig::Sodium)
target_compile_options(anof_maintenance_id_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
