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
    anof/application/ports/AuthorizationReader.h
    anof/application/ports/SpaceRepository.h
    anof/application/ports/NodeRepository.h
    anof/application/ports/QuotaRepository.h
    anof/application/ports/AuditRepository.h
    anof/application/ports/NodeIdGenerator.h
    anof/application/ports/CreateDirectoryDispatcher.h
    anof/application/ports/SessionTokenCodec.h
    anof/application/ports/RequestIdentity.h
    anof/runtime/IdentityExecutor.h
    anof/runtime/InstanceIdentity.h
    anof/runtime/AuthenticationLimiter.h
    anof/runtime/CompletionPump.h
    anof/runtime/MetadataExecutor.h
    anof/application/ports/InstancePreparation.h
    anof/application/maintenance/PrepareInstance.h
    anof/runtime/InstanceInitialization.h
    anof/runtime/InstancePasswordPolicy.h
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

# 领域与端口独立于具体后端；每个头文件独立编译。
set(contract_headers
    anof/core/Clock.h
    anof/core/Error.h
    anof/core/Id.h
    anof/core/Page.h
    anof/core/Quantity.h
    anof/core/Result.h
    anof/domain/drive/DirectoryPolicy.h
    anof/domain/drive/Node.h
    anof/domain/drive/NodeDetails.h
    anof/domain/drive/Quota.h
    anof/domain/drive/Space.h
    anof/domain/identity/AuthenticationSource.h
    anof/application/identity/AccountService.h
    anof/application/identity/AccountCommands.h
    anof/application/identity/IdentityService.h
    anof/domain/identity/IdentityEvent.h
    anof/domain/identity/IdentityValues.h
    anof/domain/identity/Invite.h
    anof/domain/identity/Principal.h
    anof/domain/identity/Secrets.h
    anof/domain/identity/Session.h
    anof/domain/identity/SessionDigest.h
    anof/domain/identity/User.h
    anof/domain/instance/InstanceSettings.h
    anof/application/RequestContext.h
    anof/application/drive/CreateDirectory.h
    anof/application/identity/IdentityCommands.h
    anof/application/maintenance/InitializeInstance.h
    anof/application/maintenance/Initialization.h
    anof/application/maintenance/PrepareInstance.h
    anof/application/ports/AuditRepository.h
    anof/application/ports/AuthorizationReader.h
    anof/application/ports/CreateDirectoryDispatcher.h
    anof/application/ports/CredentialIssuer.h
    anof/application/ports/IdentityAuditRepository.h
    anof/application/ports/IdentityDispatcher.h
    anof/application/ports/IdentityIdGenerator.h
    anof/application/ports/InstanceInitialization.h
    anof/application/ports/InstancePreparation.h
    anof/application/ports/InstanceSettingsRepository.h
    anof/application/ports/InviteRepository.h
    anof/application/ports/InviteTokenCodec.h
    anof/application/ports/MetadataReadSnapshot.h
    anof/application/ports/MetadataStore.h
    anof/application/ports/MetadataTransaction.h
    anof/application/ports/NodeIdGenerator.h
    anof/application/ports/NodeRepository.h
    anof/application/ports/PasswordHasher.h
    anof/application/ports/QuotaRepository.h
    anof/application/ports/RequestIdentity.h
    anof/application/ports/SessionRepository.h
    anof/application/ports/SessionTokenCodec.h
    anof/application/ports/SpaceRepository.h
    anof/application/ports/TaskDispatcher.h
    anof/application/ports/UserRepository.h
)
set(contract_header_sources)
foreach(header IN LISTS contract_headers)
    string(REPLACE "/" "_" source_name "${header}")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/contract_headers/${source_name}.cc")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <${header}>\n")
    list(APPEND contract_header_sources "${source}")
endforeach()
add_library(anof_contract_headers_check OBJECT ${contract_header_sources})
target_link_libraries(anof_contract_headers_check PRIVATE anof_application)

set(setup_headers
    apps/anof/Init.h
    apps/anof/TerminalPassword.h
    anof/runtime/InstanceInitialization.h
    anof/runtime/InstancePasswordPolicy.h
)
set(setup_header_sources)
foreach(header IN LISTS setup_headers)
    string(REPLACE "/" "_" source_name "${header}")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/setup_headers/${source_name}.cc")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <${header}>\n")
    list(APPEND setup_header_sources "${source}")
endforeach()
add_library(anof_setup_headers_check OBJECT ${setup_header_sources})
target_link_libraries(anof_setup_headers_check PRIVATE anof_application)

add_library(anof_identity_headers_check OBJECT
    ${CMAKE_CURRENT_BINARY_DIR}/headers/anof_runtime_IdentityExecutor.h.cc
    ${CMAKE_CURRENT_BINARY_DIR}/headers/anof_runtime_InstanceIdentity.h.cc
    ${CMAKE_CURRENT_BINARY_DIR}/headers/anof_runtime_AuthenticationLimiter.h.cc)
target_link_libraries(anof_identity_headers_check PRIVATE anof_runtime)

add_subdirectory(diagnostics)
