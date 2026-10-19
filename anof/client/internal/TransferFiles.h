#pragma once
#include <anof/client/ProfileStore.h>
#include <anof/client/internal/Json.h>
#include <sys/stat.h>
namespace anof::client::transfer_internal {
using Json = nlohmann::json;
struct Failure {
    ClientError error;
};
[[noreturn]] void fail(ErrorKind kind, std::string code = {});
void check(bool value, std::string code = "TRANSFER_LOCAL_ERROR");
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd();
    Fd(Fd&&) noexcept;
    Fd& operator=(Fd&&) noexcept;
    Fd(const Fd&) = delete;
};
bool absolute(const std::string&);
Fd directory(const std::string&, bool privateMode = false);
void safeFile(int fd);
struct stat status(int fd);
Json identity(int fd);
std::string hash(int fd, const std::atomic_bool* cancel);
std::string uuid();
void writeAll(int fd, std::string_view);
class Record {
public:
    Record(const std::string& root, const Profile&, std::optional<std::string> id = {});
    Json data;
    const std::string& id() const { return id_; }
    void save();

private:
    Fd dir_, lock_;
    std::string id_;
    bool created_ = false;
};
}  // namespace anof::client::transfer_internal
