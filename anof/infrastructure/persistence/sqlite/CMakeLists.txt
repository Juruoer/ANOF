set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    migrations/0001_create_directory.sql
)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0001_create_directory.sql" ANOF_SQLITE_MIGRATION)
configure_file(SqliteMigrationSql.h.in generated/SqliteMigrationSql.h @ONLY)

add_library(anof_infrastructure STATIC
    SqliteInstanceMaintenance.h
    SqliteInstanceMaintenance.cc
    SqliteConnection.cc
    SqliteStatement.cc
    SqliteValue.cc
    SqliteError.cc
    SqliteMigration.cc
    SqliteMetadataStore.cc
    SqliteMetadataTransaction.cc
    SqliteTransactionContext.cc
    SqliteAuthorizationRepository.cc
    SqliteSpaceRepository.cc
    SqliteNodeRepository.cc
    SqliteQuotaRepository.cc
    SqliteAuditRepository.cc
)
target_link_libraries(anof_infrastructure PUBLIC anof_application PRIVATE SQLite::SQLite3 nlohmann_json::nlohmann_json)
target_include_directories(anof_infrastructure PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(anof_infrastructure PUBLIC cxx_std_17)
target_compile_options(anof_infrastructure PRIVATE -Wall -Wextra -Wconversion -Wshadow)

if (ANOF_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
