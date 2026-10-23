add_executable(anof_sqlite_foundation_unittest SqliteFoundation_unittest.cc)
target_link_libraries(anof_sqlite_foundation_unittest PRIVATE
    anof_sqlite_foundation SQLite::SQLite3 Boost::unit_test_framework
)
target_compile_definitions(anof_sqlite_foundation_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_foundation_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_foundation_unittest COMMAND anof_sqlite_foundation_unittest)
set_tests_properties(anof_sqlite_foundation_unittest PROPERTIES TIMEOUT 30 LABELS "unit;sqlite")

add_executable(anof_sqlite_transaction_unittest SqliteMetadataTransaction_unittest.cc)
target_link_libraries(anof_sqlite_transaction_unittest PRIVATE
    anof_infrastructure SQLite::SQLite3 Boost::unit_test_framework
)
target_compile_definitions(anof_sqlite_transaction_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_transaction_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_transaction_unittest COMMAND anof_sqlite_transaction_unittest)
set_tests_properties(anof_sqlite_transaction_unittest PROPERTIES TIMEOUT 30 LABELS "unit;sqlite")

add_executable(anof_sqlite_node_unittest SqliteNodeRepository_unittest.cc)
target_link_libraries(anof_sqlite_node_unittest PRIVATE
    anof_infrastructure SQLite::SQLite3 Boost::unit_test_framework
)
target_compile_definitions(anof_sqlite_node_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_node_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_node_unittest COMMAND anof_sqlite_node_unittest)
set_tests_properties(anof_sqlite_node_unittest PROPERTIES TIMEOUT 30 LABELS "unit;sqlite")

anof_add_persistence_contract_test(
    anof_sqlite_contract_unittest
    SqliteTestMetadataBackend.cc
    anof_infrastructure
)
target_compile_options(anof_sqlite_contract_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

add_executable(anof_sqlite_quota_audit_unittest
    SqliteQuotaAudit_unittest.cc
    SqliteTestMetadataBackend.cc
)
target_link_libraries(anof_sqlite_quota_audit_unittest PRIVATE
    anof_infrastructure SQLite::SQLite3 Boost::unit_test_framework
)
target_compile_definitions(anof_sqlite_quota_audit_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_quota_audit_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_quota_audit_unittest COMMAND anof_sqlite_quota_audit_unittest)
set_tests_properties(anof_sqlite_quota_audit_unittest PROPERTIES TIMEOUT 30 LABELS "unit;sqlite")

add_executable(anof_sqlite_acceptance_unittest
    SqliteAcceptance_unittest.cc
    SqliteFaultVfs.cc
    SqliteTestMetadataBackend.cc
)
target_link_libraries(anof_sqlite_acceptance_unittest PRIVATE
    anof_infrastructure SQLite::SQLite3 Boost::unit_test_framework
)
target_compile_definitions(anof_sqlite_acceptance_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_acceptance_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_acceptance_unittest COMMAND anof_sqlite_acceptance_unittest)
set_tests_properties(anof_sqlite_acceptance_unittest PROPERTIES TIMEOUT 30 LABELS "unit;sqlite")

# 本模块头文件独立编译，不修改根会话维护的公共头文件清单。
set(sqlite_headers
    SqliteInstanceBootstrap.h
    SqliteInitializedState.h
    SqliteInstanceFiles.h
    SqliteBootstrapInternal.h
    SqliteInstanceMaintenance.h
    SqliteAuditRepository.h
    SqliteAuthorizationReader.h
    SqliteConnection.h
    SqliteError.h
    SqliteMetadataStore.h
    SqliteMetadataStoreInternal.h
    SqliteMetadataReadSnapshot.h
    SqliteMetadataTransaction.h
    SqliteSchema.h
    SqliteFunctions.h
    SqliteNodeRepository.h
    SqliteOptions.h
    SqliteQuotaRepository.h
    SqliteSpaceRepository.h
    SqliteStatement.h
    SqliteTransactionContext.h
    SqliteValue.h
)
set(sqlite_header_sources)
foreach(header IN LISTS sqlite_headers)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cc")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <anof/infrastructure/persistence/sqlite/${header}>\n")
    list(APPEND sqlite_header_sources "${source}")
endforeach()
add_library(anof_sqlite_headers_check OBJECT ${sqlite_header_sources})
target_link_libraries(anof_sqlite_headers_check PRIVATE anof_infrastructure)

add_executable(anof_sqlite_instance_maintenance_unittest SqliteInstanceMaintenance_unittest.cc)
target_link_libraries(anof_sqlite_instance_maintenance_unittest PRIVATE
    anof_infrastructure SQLite::SQLite3 Boost::unit_test_framework
)
target_compile_definitions(anof_sqlite_instance_maintenance_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_instance_maintenance_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_instance_maintenance_unittest COMMAND anof_sqlite_instance_maintenance_unittest)
set_tests_properties(anof_sqlite_instance_maintenance_unittest PROPERTIES TIMEOUT 30 LABELS "unit;sqlite;instance")

add_executable(anof_sqlite_instance_preparation_unittest InstancePreparation_unittest.cc)
target_link_libraries(anof_sqlite_instance_preparation_unittest PRIVATE
    anof_infrastructure Threads::Threads Boost::unit_test_framework
)
target_compile_definitions(anof_sqlite_instance_preparation_unittest PRIVATE
    BOOST_TEST_DYN_LINK
    INSTANCE_LOCK_PROBE="$<TARGET_FILE:anof_instance_lock_probe>"
)
target_compile_options(anof_sqlite_instance_preparation_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_dependencies(anof_sqlite_instance_preparation_unittest anof_instance_lock_probe)
add_test(NAME anof_sqlite_instance_preparation_unittest COMMAND anof_sqlite_instance_preparation_unittest)
set_tests_properties(anof_sqlite_instance_preparation_unittest PROPERTIES TIMEOUT 30 LABELS "integration;sqlite;instance")

set(foundation_headers SqliteSchema.h SqliteConnection.h SqliteStatement.h SqliteValue.h SqliteError.h SqliteFunctions.h SqliteOptions.h)
set(foundation_header_sources)
foreach(header IN LISTS foundation_headers)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/foundation_headers/${header}.cc")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <anof/infrastructure/persistence/sqlite/${header}>\n")
    list(APPEND foundation_header_sources "${source}")
endforeach()
add_library(anof_sqlite_foundation_headers_check OBJECT ${foundation_header_sources})
target_link_libraries(anof_sqlite_foundation_headers_check PRIVATE anof_application)

add_executable(anof_sqlite_identity_unittest SqliteIdentity_unittest.cc)
target_link_libraries(anof_sqlite_identity_unittest PRIVATE anof_sqlite_identity SQLite::SQLite3 Boost::unit_test_framework)
target_compile_definitions(anof_sqlite_identity_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_identity_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_identity_unittest COMMAND anof_sqlite_identity_unittest)
set_tests_properties(anof_sqlite_identity_unittest PROPERTIES TIMEOUT 30 LABELS "unit;sqlite;identity")

set(identity_headers SqliteAuthorizationReader.h SqliteUserRepository.h SqliteSessionRepository.h SqliteInviteRepository.h SqliteSettingsRepository.h SqliteIdentityAuditRepository.h SqliteIdentitySql.h SqliteTransactionContext.h)
set(identity_header_sources)
foreach(header IN LISTS identity_headers)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/identity_headers/${header}.cc")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <anof/infrastructure/persistence/sqlite/${header}>\n")
    list(APPEND identity_header_sources "${source}")
endforeach()
add_library(anof_sqlite_identity_headers_check OBJECT ${identity_header_sources})
target_link_libraries(anof_sqlite_identity_headers_check PRIVATE anof_application)

add_executable(anof_sqlite_read_snapshot_unittest SqliteReadSnapshot_unittest.cc SqliteTestMetadataBackend.cc)
target_link_libraries(anof_sqlite_read_snapshot_unittest PRIVATE anof_infrastructure SQLite::SQLite3 Boost::unit_test_framework)
target_compile_definitions(anof_sqlite_read_snapshot_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_read_snapshot_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_read_snapshot_unittest COMMAND anof_sqlite_read_snapshot_unittest)
set_tests_properties(anof_sqlite_read_snapshot_unittest PROPERTIES TIMEOUT 30 LABELS "unit;sqlite")

add_executable(anof_sqlite_instance_bootstrap_unittest SqliteInstanceBootstrap_unittest.cc SqliteFaultVfs.cc)
target_link_libraries(anof_sqlite_instance_bootstrap_unittest PRIVATE anof_infrastructure SQLite::SQLite3 Boost::unit_test_framework)
target_compile_definitions(anof_sqlite_instance_bootstrap_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_instance_bootstrap_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_instance_bootstrap_unittest COMMAND anof_sqlite_instance_bootstrap_unittest)
set_tests_properties(anof_sqlite_instance_bootstrap_unittest PROPERTIES TIMEOUT 30 LABELS "unit;sqlite;instance")

add_executable(anof_sqlite_m2_boundaries_unittest SqliteM2Boundaries_unittest.cc)
target_link_libraries(anof_sqlite_m2_boundaries_unittest PRIVATE anof_infrastructure Threads::Threads Boost::unit_test_framework)
target_compile_definitions(anof_sqlite_m2_boundaries_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_m2_boundaries_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_m2_boundaries_unittest COMMAND anof_sqlite_m2_boundaries_unittest)
set_tests_properties(anof_sqlite_m2_boundaries_unittest PROPERTIES TIMEOUT 45 LABELS "unit;sqlite;instance;concurrency")

anof_add_transfer_contract_test(anof_sqlite_transfer_contract_unittest SqliteTestTransferBackend.cc anof_infrastructure)
target_sources(anof_sqlite_transfer_contract_unittest PRIVATE SqliteTestMetadataBackend.cc)
add_executable(anof_sqlite_transfer_unittest SqliteTransfer_unittest.cc SqliteTestTransferBackend.cc SqliteTestMetadataBackend.cc)
target_link_libraries(anof_sqlite_transfer_unittest PRIVATE anof_infrastructure SQLite::SQLite3 Boost::unit_test_framework)
target_compile_definitions(anof_sqlite_transfer_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_transfer_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_transfer_unittest COMMAND anof_sqlite_transfer_unittest)
set_tests_properties(anof_sqlite_transfer_unittest PROPERTIES TIMEOUT 45 LABELS "unit;sqlite;transfer")
set(transfer_headers SqliteTreeRepository.h SqliteTrashRepository.h SqliteNodeAccountingRepository.h SqliteDriveAuditRepository.h SqliteUploadRepository.h SqliteBlobRepository.h SqliteStorageRepository.h SqliteTransferAuditRepository.h)
set(transfer_header_sources)
foreach(header IN LISTS transfer_headers)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/transfer_headers/${header}.cc")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <anof/infrastructure/persistence/sqlite/${header}>\n")
    list(APPEND transfer_header_sources "${source}")
endforeach()
add_library(anof_sqlite_transfer_headers_check OBJECT ${transfer_header_sources})
target_link_libraries(anof_sqlite_transfer_headers_check PRIVATE anof_application)

add_executable(anof_sqlite_drive_contract_unittest SqliteTestDriveBackend.cc SqliteTestMetadataBackend.cc $<TARGET_OBJECTS:anof_drive_contract_cases>)
target_link_libraries(anof_sqlite_drive_contract_unittest PRIVATE anof_infrastructure SQLite::SQLite3 Boost::unit_test_framework)
target_compile_definitions(anof_sqlite_drive_contract_unittest PRIVATE BOOST_TEST_DYN_LINK)
add_test(NAME anof_sqlite_drive_contract_unittest COMMAND anof_sqlite_drive_contract_unittest)
set_tests_properties(anof_sqlite_drive_contract_unittest PROPERTIES TIMEOUT 45 LABELS "sqlite;drive;persistence_contract")
add_executable(anof_sqlite_drive_unittest SqliteDrive_unittest.cc SqliteTestDriveBackend.cc SqliteTestMetadataBackend.cc)
target_link_libraries(anof_sqlite_drive_unittest PRIVATE anof_infrastructure SQLite::SQLite3 Boost::unit_test_framework)
target_compile_definitions(anof_sqlite_drive_unittest PRIVATE BOOST_TEST_DYN_LINK)
target_compile_options(anof_sqlite_drive_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
add_test(NAME anof_sqlite_drive_unittest COMMAND anof_sqlite_drive_unittest)
set_tests_properties(anof_sqlite_drive_unittest PROPERTIES TIMEOUT 45 LABELS "unit;sqlite;drive")
