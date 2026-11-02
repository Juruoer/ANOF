set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS schema/current.sql)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/schema/current.sql" ANOF_SQLITE_SCHEMA)
configure_file(SqliteSchemaSql.h.in generated/SqliteSchemaSql.h @ONLY)

# 复用同一基础实现，低层校验可独立构建，不依赖尚在适配的仓储或 transport。
add_library(anof_sqlite_foundation STATIC
        SqliteConnection.cc
        SqliteStatement.cc
        SqliteValue.cc
        SqliteError.cc
        SqliteFunctions.cc
        SqliteSchema.cc
)
target_link_libraries(anof_sqlite_foundation PUBLIC anof_application PRIVATE SQLite::SQLite3)
target_include_directories(anof_sqlite_foundation PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(anof_sqlite_foundation PUBLIC cxx_std_17)
target_compile_options(anof_sqlite_foundation PRIVATE -Wall -Wextra -Wconversion -Wshadow)

add_library(anof_sqlite_identity STATIC
        SqliteDriveIntegrity.cc
        SqliteTransactionContext.cc
        SqliteIdentitySql.cc
        SqliteAuthorizationReader.cc
        SqliteUserRepository.cc
        SqliteSessionRepository.cc
        SqliteInviteRepository.cc
        SqliteSettingsRepository.cc
        SqliteIdentityAuditRepository.cc
)
target_link_libraries(anof_sqlite_identity PUBLIC anof_sqlite_foundation PRIVATE SQLite::SQLite3)
target_compile_features(anof_sqlite_identity PUBLIC cxx_std_17)
target_compile_options(anof_sqlite_identity PRIVATE -Wall -Wextra -Wconversion -Wshadow)

add_library(anof_infrastructure STATIC
        SqliteMaintenanceRead.cc
        SqliteMetadataSnapshot.cc
        internal/RepairInvariant.cc
        SqliteMetadataInspection.cc
        SqliteAuditRetention.cc
        SqliteMetadataRepair.cc
        internal/MaintenanceCodec.cc
        internal/MaintenanceSql.cc
        SqliteInstanceMaintenance.h
        SqliteInstanceMaintenance.cc
        SqliteInstanceBootstrap.h
        SqliteInstanceBootstrap.cc
        SqliteInitializedState.h
        SqliteInitializedState.cc
        SqliteInstanceFiles.h
        SqliteBootstrapInternal.h
        SqliteInstanceFiles.cc
        SqliteTreeRepository.cc
        SqliteTrashRepository.cc
        SqliteNodeAccountingRepository.cc
        SqliteDriveAuditRepository.cc
        SqliteBlobLifecycle.cc
        SqliteUploadRepository.cc
        SqliteTransferRepositories.cc
        SqliteMetadataStore.cc
        SqliteMaintenanceStateReader.cc
        SqliteMetadataReadSnapshot.cc
        SqliteMetadataTransaction.cc
        SqliteSpaceRepository.cc
        SqliteNodeRepository.cc
        SqliteQuotaRepository.cc
        SqliteAuditRepository.cc
)
target_link_libraries(anof_infrastructure PUBLIC anof_application PRIVATE anof_sqlite_identity SQLite::SQLite3 nlohmann_json::nlohmann_json)
target_include_directories(anof_infrastructure PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(anof_infrastructure PUBLIC cxx_std_17)
target_compile_options(anof_infrastructure PRIVATE -Wall -Wextra -Wconversion -Wshadow)

if (ANOF_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
