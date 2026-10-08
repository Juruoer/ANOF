add_subdirectory(persistence_contract)

# 每个公共头文件单独编译，防止依赖其他头文件的偶然 include 顺序。
set(public_headers
    apps/anof/ShutdownSignals.h
    anof/core/Error.h
    anof/core/Result.h
    anof/core/Id.h
    anof/core/Quantity.h
    anof/core/Clock.h
    anof/domain/identity/Principal.h
    anof/domain/identity/SessionDigest.h
    anof/domain/drive/Node.h
    anof/domain/drive/Space.h
    anof/domain/drive/Quota.h
    anof/domain/drive/DirectoryPolicy.h
    anof/application/drive/CreateDirectory.h
    anof/application/ports/MetadataStore.h
    anof/application/ports/MetadataTransaction.h
    anof/application/ports/AuthorizationRepository.h
    anof/application/ports/SpaceRepository.h
    anof/application/ports/NodeRepository.h
    anof/application/ports/QuotaRepository.h
    anof/application/ports/AuditRepository.h
    anof/application/ports/NodeIdGenerator.h
    anof/application/ports/CreateDirectoryDispatcher.h
    anof/application/ports/SessionTokenCodec.h
    anof/application/ports/RequestIdentity.h
    anof/runtime/CompletionPump.h
    anof/runtime/MetadataExecutor.h
    anof/application/ports/InstancePreparation.h
    anof/application/maintenance/PrepareInstance.h
    anof/runtime/InstanceServer.h
    anof/runtime/AnofRuntime.h
    tests/persistence_contract/TestMetadataBackend.h
)
set(header_sources)
foreach(header IN LISTS public_headers)
    string(REPLACE "/" "_" source_name "${header}")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${source_name}.cc")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <${header}>\n")
    list(APPEND header_sources "${source}")
endforeach()
add_library(anof_public_headers_check OBJECT ${header_sources})
target_link_libraries(anof_public_headers_check PRIVATE anof_application anof_instance_runtime)

add_subdirectory(integration)
