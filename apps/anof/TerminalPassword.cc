#include <apps/anof/TerminalPassword.h>
#include <array>
#include <optional>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/signalfd.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>

namespace anof::app {
namespace {
struct Buffer {
    std::array<char, 1024> bytes{};
    ~Buffer() { volatile char* p = bytes.data(); for (std::size_t i = 0; i < bytes.size(); ++i) p[i] = 0; }
};
struct Terminal {
    explicit Terminal(std::atomic_bool* flag = nullptr) : cancelled(flag) {}
    std::atomic_bool* cancelled;
    int fd = -1, signals = -1;
    termios original{};
    sigset_t originalMask{};
    bool changed = false, masked = false;
    bool restore() noexcept {
        if (!changed) return true;
        int result;
        do { result = ::tcsetattr(fd, TCSAFLUSH, &original); } while (result != 0 && errno == EINTR);
        if (result == 0) changed = false;
        return result == 0;
    }
    ~Terminal() {
        (void)restore();
        if (signals >= 0) {
            signalfd_siginfo pending{};
            while (::read(signals, &pending, sizeof pending) > 0) { if (cancelled) cancelled->store(true, std::memory_order_relaxed); }
            ::close(signals);
        }
        if (fd >= 0) ::close(fd);
        if (masked) (void)::pthread_sigmask(SIG_SETMASK, &originalMask, nullptr);
    }
    bool open() {
        fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
        if (fd < 0 || ::tcgetattr(fd, &original) != 0) return false;
        sigset_t mask;
        ::sigemptyset(&mask);
        for (int s : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP}) ::sigaddset(&mask, s);
        if (::pthread_sigmask(SIG_BLOCK, &mask, &originalMask) != 0) return false;
        masked = true;
        signals = ::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
        if (signals < 0) return false;
        auto hidden = original;
        hidden.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL));
        hidden.c_lflag |= ICANON;
        if (::tcsetattr(fd, TCSAFLUSH, &hidden) != 0) return false;
        changed = true;
        return true;
    }
    bool prompt(const char* text, std::size_t size) {
        while (size) {
            const auto n = ::write(fd, text, size);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            text += n; size -= static_cast<std::size_t>(n);
        }
        return true;
    }
    TerminalResult<detail::SecretStorage> read(std::size_t limit = 1024) {
        Buffer buffer;
        std::size_t size = 0;
        bool overflow = false;
        for (;;) {
            if (cancelled && cancelled->load(std::memory_order_relaxed)) return TerminalError::Interrupted;
            pollfd descriptors[2]{{fd, POLLIN, 0}, {signals, POLLIN, 0}};
            const int ready = ::poll(descriptors, 2, -1);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) return TerminalError::IoError;
            if (descriptors[1].revents) return TerminalError::Interrupted;
            if (!(descriptors[0].revents & POLLIN)) return TerminalError::IoError;
            // 单字节有界缓冲；秘密不进入 std::string，超限仍排空当前行再拒绝。
            char byte = 0;
            const auto n = ::read(fd, &byte, 1);
            if (n < 0 && errno == EINTR) continue;
            if (n != 1) return TerminalError::InvalidInput;
            if (byte == '\n') break;
            if (size == limit || size == buffer.bytes.size()) overflow = true;
            else buffer.bytes[size++] = byte;
            *static_cast<volatile char*>(&byte) = 0;
        }
        if (overflow) return TerminalError::InvalidInput;
        return detail::SecretStorage({buffer.bytes.data(), size});
    }
};
}
namespace {
TerminalResult<PasswordInput> password(Terminal& terminal, const char* prompt, std::size_t length, bool confirm) {
    if (!terminal.prompt(prompt, length)) return TerminalError::IoError;
    auto input = terminal.read();
    if (auto* error = std::get_if<TerminalError>(&input)) return *error;
    auto first = PasswordInput::parse(std::get<detail::SecretStorage>(input).view());
    if (!first) return TerminalError::InvalidInput;
    if (confirm) {
        constexpr char again[] = "\nConfirm password: ";
        if (!terminal.prompt(again, sizeof again - 1)) return TerminalError::IoError;
        auto second = terminal.read();
        if (auto* error = std::get_if<TerminalError>(&second)) return *error;
        if (first.value().view() != std::get<detail::SecretStorage>(second).view()) return TerminalError::InvalidInput;
    }
    if (!terminal.prompt("\n", 1)) return TerminalError::IoError;
    return std::move(first).value();
}
std::optional<TerminalError> finish(Terminal& terminal) {
    if (terminal.cancelled && terminal.cancelled->load(std::memory_order_relaxed)) return TerminalError::Interrupted;
    pollfd signal{terminal.signals, POLLIN, 0};
    if (::poll(&signal, 1, 0) > 0 && signal.revents) return TerminalError::Interrupted;
    if (!terminal.restore()) return TerminalError::IoError;
    return {};
}
}
Result<PasswordInput> readConfirmedPassword() {
    Terminal terminal;
    if (!terminal.open()) return ErrorCode::InvalidArgument;
    constexpr char prompt[] = "Administrator password: ";
    auto result = password(terminal, prompt, sizeof prompt - 1, true);
    if (std::holds_alternative<TerminalError>(result) || finish(terminal)) return ErrorCode::InvalidArgument;
    return std::move(std::get<PasswordInput>(result));
}
TerminalResult<PasswordInput> readLoginPassword(std::atomic_bool* cancelled) {
    Terminal terminal(cancelled);
    if (!terminal.open()) return TerminalError::Unavailable;
    constexpr char prompt[] = "Password: ";
    auto result = password(terminal, prompt, sizeof prompt - 1, false);
    if (auto error = finish(terminal)) return *error;
    return result;
}
TerminalResult<RegistrationSecrets> readRegistrationSecrets(std::atomic_bool* cancelled) {
    Terminal terminal(cancelled);
    if (!terminal.open()) return TerminalError::Unavailable;
    constexpr char prompt[] = "Invitation token: ";
    if (!terminal.prompt(prompt, sizeof prompt - 1)) return TerminalError::IoError;
    auto input = terminal.read(55);
    if (auto* error = std::get_if<TerminalError>(&input)) return *error;
    auto invite = InviteTokenText::parse(std::get<detail::SecretStorage>(input).view());
    if (!invite) return TerminalError::InvalidInput;
    constexpr char newPassword[] = "\nNew password: ";
    auto secret = password(terminal, newPassword, sizeof newPassword - 1, true);
    if (auto* error = std::get_if<TerminalError>(&secret)) return *error;
    if (auto error = finish(terminal)) return *error;
    return RegistrationSecrets{std::move(std::get<PasswordInput>(secret)), std::move(invite).value()};
}
TerminalResult<PasswordChangeSecrets> readPasswordChangeSecrets(std::atomic_bool* cancelled) {
    Terminal terminal(cancelled);
    if (!terminal.open()) return TerminalError::Unavailable;
    constexpr char oldPrompt[] = "Current password: ";
    auto current = password(terminal, oldPrompt, sizeof oldPrompt - 1, false);
    if (auto* error = std::get_if<TerminalError>(&current)) return *error;
    constexpr char newPrompt[] = "New password: ";
    auto next = password(terminal, newPrompt, sizeof newPrompt - 1, true);
    if (auto* error = std::get_if<TerminalError>(&next)) return *error;
    if (auto error = finish(terminal)) return *error;
    return PasswordChangeSecrets{std::move(std::get<PasswordInput>(current)), std::move(std::get<PasswordInput>(next))};
}
}  // namespace anof::app
