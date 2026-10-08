target_sources(anof_infrastructure PRIVATE
    SodiumSessionTokenCodec.cc
    SodiumRequestIdentity.cc
    SodiumNodeIdGenerator.cc
    internal/NodeUuid.cc
    internal/NodeUuid.h
    internal/RequestIdDerivation.cc
    internal/RequestIdDerivation.h
    internal/SodiumSupport.cc
    internal/SodiumSupport.h
    SodiumSessionTokenCodec.h
    SodiumRequestIdentity.h
    SodiumNodeIdGenerator.h
)
target_link_libraries(anof_infrastructure PRIVATE PkgConfig::Sodium)

if (ANOF_BUILD_TESTS)
    set(crypto_headers
        SodiumSessionTokenCodec.h
        SodiumRequestIdentity.h
        SodiumNodeIdGenerator.h
    )
    set(crypto_header_sources)
    foreach(header IN LISTS crypto_headers)
        set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cc")
        file(GENERATE OUTPUT "${source}" CONTENT "#include <anof/infrastructure/crypto/${header}>\n")
        list(APPEND crypto_header_sources "${source}")
    endforeach()
    add_library(anof_crypto_headers_check OBJECT ${crypto_header_sources})
    target_link_libraries(anof_crypto_headers_check PRIVATE anof_application)
    add_subdirectory(tests)
endif ()
