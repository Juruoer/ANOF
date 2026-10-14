#pragma once
#include <anof/domain/identity/Secrets.h>
#include <variant>
#include <atomic>

namespace anof::app {
// 仅从自有 /dev/tty 读取两次，无 argv/env/stdin 密码入口；中断时恢复终端设置。
Result<PasswordInput> readConfirmedPassword();
enum class TerminalError { Unavailable, InvalidInput, Interrupted, IoError };
template<class T> using TerminalResult = std::variant<T, TerminalError>;
struct PasswordChangeSecrets { PasswordInput current; PasswordInput next; };
TerminalResult<PasswordChangeSecrets> readPasswordChangeSecrets(std::atomic_bool* cancelled = nullptr);
struct RegistrationSecrets { PasswordInput password; InviteTokenText invite; };
TerminalResult<PasswordInput> readLoginPassword(std::atomic_bool* cancelled = nullptr);
TerminalResult<RegistrationSecrets> readRegistrationSecrets(std::atomic_bool* cancelled = nullptr);
}  // namespace anof::app
