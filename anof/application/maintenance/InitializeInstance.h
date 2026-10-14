#pragma once
#include <anof/application/ports/InstanceInitialization.h>
#include <anof/application/ports/IdentityIdGenerator.h>
#include <anof/application/ports/PasswordHasher.h>

namespace anof {
class InitializeInstance {
public:
    InitializeInstance(InstanceInitializer& files, InstanceBootstrap& database, PasswordHasher& passwords,
                       IdentityIdGenerator& ids, Clock& clock)
        : files_(files), database_(database), passwords_(passwords), ids_(ids), clock_(clock) {}
    InitResult<InitReceipt> execute(InitOptions options, PasswordInput password, RequestId requestId);
private:
    InstanceInitializer& files_;
    InstanceBootstrap& database_;
    PasswordHasher& passwords_;
    IdentityIdGenerator& ids_;
    Clock& clock_;
};
class FinishInitialization {
public:
    FinishInitialization(InstanceInitializer& files, InstanceBootstrap& database) : files_(files), database_(database) {}
    InitResult<InitReceipt> execute(const std::filesystem::path& root);
private:
    InstanceInitializer& files_;
    InstanceBootstrap& database_;
};
}  // namespace anof
