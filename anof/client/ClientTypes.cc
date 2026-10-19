#include <anof/client/ClientTypes.h>
#include <cstring>
#include <utility>
namespace anof::client {
int ClientError::exitCode() const noexcept {
    switch (kind) {
    case ErrorKind::InvalidArgument: return 2;
    case ErrorKind::AuthenticationRequired: case ErrorKind::Application: return 3;
    case ErrorKind::Transport: case ErrorKind::Protocol: return 4;
    case ErrorKind::LocalState: return 5;
    case ErrorKind::OutcomeUnknown: return 6;
    case ErrorKind::Interrupted: return 130;
    }
    return 5;
}
ClientError failure(ErrorKind kind, std::string code) {
    if (code.empty()) {
        switch (kind) {
        case ErrorKind::InvalidArgument: code = "INVALID_ARGUMENT"; break;
        case ErrorKind::AuthenticationRequired: code = "AUTHENTICATION_REQUIRED"; break;
        case ErrorKind::Application: code = "APPLICATION_ERROR"; break;
        case ErrorKind::Transport: code = "TRANSPORT_ERROR"; break;
        case ErrorKind::Protocol: code = "PROTOCOL_ERROR"; break;
        case ErrorKind::LocalState: code = "LOCAL_STATE_ERROR"; break;
        case ErrorKind::OutcomeUnknown: code = "OUTCOME_UNKNOWN"; break;
        case ErrorKind::Interrupted: code = "INTERRUPTED"; break;
        }
    }
    return {kind, std::move(code), {}, {}, (kind == ErrorKind::OutcomeUnknown || kind == ErrorKind::Protocol) ? RemoteOutcome::Unknown : RemoteOutcome::NotSent, LocalState::Unchanged, {}, {}};
}
const char* errorMessage(ErrorKind kind) noexcept {
    switch (kind) {
    case ErrorKind::InvalidArgument: return "Invalid client argument";
    case ErrorKind::AuthenticationRequired: return "Local credentials required";
    case ErrorKind::Application: return "Server rejected the operation";
    case ErrorKind::Transport: return "HTTP transport failed";
    case ErrorKind::Protocol: return "Invalid server response";
    case ErrorKind::LocalState: return "Local state operation failed";
    case ErrorKind::OutcomeUnknown: return "Remote outcome unknown; no automatic retry";
    case ErrorKind::Interrupted: return "Operation interrupted";
    }
    return "Client operation failed";
}
SecureBuffer::SecureBuffer(std::size_t capacity) : bytes_(capacity ? std::make_unique<char[]>(capacity) : nullptr), capacity_(capacity) {}
SecureBuffer::~SecureBuffer() { clear(); }
SecureBuffer::SecureBuffer(SecureBuffer&& other) noexcept : bytes_(std::move(other.bytes_)), capacity_(std::exchange(other.capacity_, 0)), size_(std::exchange(other.size_, 0)) {}
SecureBuffer& SecureBuffer::operator=(SecureBuffer&& other) noexcept {
    if (this != &other) { clear(); bytes_ = std::move(other.bytes_); capacity_ = std::exchange(other.capacity_, 0); size_ = std::exchange(other.size_, 0); }
    return *this;
}
bool SecureBuffer::append(std::string_view text) noexcept {
    if (text.size() > capacity_ - size_) return false;
    if (!text.empty()) std::memcpy(bytes_.get() + size_, text.data(), text.size());
    size_ += text.size(); return true;
}
std::string_view SecureBuffer::view() const noexcept { return size_ ? std::string_view(bytes_.get(), size_) : std::string_view{}; }
void SecureBuffer::clear() noexcept { volatile char* p = bytes_.get(); for (std::size_t i = 0; i < size_; ++i) p[i] = 0; size_ = 0; }
}  // namespace anof::client
