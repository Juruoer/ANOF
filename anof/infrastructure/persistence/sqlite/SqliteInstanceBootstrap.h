#pragma once

#include <anof/application/ports/InstanceInitialization.h>
#include <anof/infrastructure/persistence/sqlite/SqliteOptions.h>

#include <memory>

namespace anof::infrastructure::sqlite {

struct SqliteBootstrapOptions {
    // 显式持久化编码读取预算；不执行密码哈希，不能用无限上限代替验证。
    PasswordHashLimits passwordHashLimits{};
    SqliteOptions connection;
};

// 工厂不建库。无效选项为 InvalidInput/Validate。
InitResult<std::unique_ptr<InstanceBootstrap>> makeSqliteInstanceBootstrap(const SqliteBootstrapOptions& options);

}  // namespace anof::infrastructure::sqlite
