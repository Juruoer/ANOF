add_subdirectory(persistence)
add_subdirectory(crypto)
add_subdirectory(system)
add_subdirectory(config)

if (ANOF_BUILD_TESTS)
    # 新工厂仅检查声明自包含，不依赖仍在适配中的 SQLite 实现或 transport。
    set(m2_adapter_headers
        crypto/SodiumPasswordPolicy.h
        crypto/SodiumPasswordHasher.h
        crypto/SodiumCredentialIssuer.h
        crypto/SodiumInviteTokenCodec.h
        crypto/SodiumIdentityIdGenerator.h
        system/LinuxInstanceInitializer.h
        persistence/sqlite/SqliteInstanceBootstrap.h
    )
    set(m2_adapter_header_sources)
    foreach(header IN LISTS m2_adapter_headers)
        set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cc")
        file(GENERATE OUTPUT "${source}" CONTENT "#include <anof/infrastructure/${header}>\n")
        list(APPEND m2_adapter_header_sources "${source}")
    endforeach()
    add_library(anof_m2_adapter_headers_check OBJECT ${m2_adapter_header_sources})
    target_link_libraries(anof_m2_adapter_headers_check PRIVATE anof_application)
endif ()
