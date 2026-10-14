#pragma once
#include <anof/client/HttpTransport.h>
#include <functional>
namespace anof::client {
struct StoredCredential { SecretToken accessToken; std::string expiresAt; UserId userId; LoginName login; };
struct Profile { std::string name; Origin server; std::optional<StoredCredential> credential; };
struct SessionSummary { std::string expiresAt; UserId userId; LoginName login; };
struct ProfileSummary { std::string name, server; bool developmentHttp; std::optional<SessionSummary> session; };
// 注入故障只用于验证原子发布的前/后边界，不替换文件安全检查。
enum class SavePoint { BeforePublish, AfterPublish };
using SaveHook = std::function<bool(SavePoint)>;
class LockedProfile {
public:
    ~LockedProfile();
    LockedProfile(LockedProfile&&) noexcept;
    LockedProfile& operator=(LockedProfile&&) noexcept;
    LockedProfile(const LockedProfile&) = delete;
    const Profile& profile() const noexcept;
    ProfileSummary summary() const;
    bool usable() const noexcept;
    ClientResult<Done> saveCredential(StoredCredential credential);
    ClientResult<Done> clearCredential();
private:
    struct State;
    explicit LockedProfile(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
    friend class ProfileStore;
};
class ProfileStore {
public:
    static ClientResult<std::string> resolveDirectory(std::optional<std::string> explicitPath = {});
    static bool validName(std::string_view name) noexcept;
    explicit ProfileStore(std::string directory, SaveHook hook = {});
    ClientResult<Done> add(std::string name, const Origin& server);
    ClientResult<LockedProfile> lock(std::string name) const;
    ClientResult<ProfileSummary> show(std::string name) const;
    ClientResult<std::vector<ProfileSummary>> list() const;
    ClientResult<Done> remove(std::string name);
private:
    std::string directory_;
    SaveHook hook_;
};
}  // namespace anof::client
