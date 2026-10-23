#pragma once
#include <anof/client/ApiClient.h>
#include <anof/client/DriveProtocol.h>
#include <nlohmann/json.hpp>
namespace anof::app {
// 仅允许由受检 DTO 构造输出；不接受原始远端 JSON。
nlohmann::json clientData(const client::User&);
nlohmann::json clientData(const client::AdminUser&);
nlohmann::json clientData(const client::ManagedQuota&);
nlohmann::json clientData(const client::SavedLogin&);
nlohmann::json clientData(const client::Invite&);
nlohmann::json clientData(const client::Space&);
nlohmann::json clientData(const client::Node&);
nlohmann::json clientData(const client::Trash&);
nlohmann::json clientData(const client::ProfileSummary&);
nlohmann::json clientData(client::Done);
template<class T> nlohmann::json clientData(const std::vector<T>& values) {
    auto items = nlohmann::json::array(); for (const auto& value : values) items.push_back(clientData(value)); return items;
}
template<class T> nlohmann::json clientData(const client::Page<T>& page) {
    return {{"items",clientData(page.items)},{"next_cursor",page.nextCursor ? nlohmann::json(*page.nextCursor) : nlohmann::json(nullptr)}};
}
class ClientOutput {
public:
    explicit ClientOutput(bool json, std::optional<std::string> profile = {}) : json_(json), profile_(std::move(profile)) {}
    void profile(std::optional<std::string> name) { profile_ = std::move(name); }
    int error(const client::ClientError&) const noexcept;
    int help(std::string_view text) const;
    int success(const nlohmann::json&, std::optional<client::ResponseMeta> meta = {}, std::string_view invite = {}) const;
    std::optional<std::string> transferId;
    std::optional<std::string> operation, sourceId;
    std::string notice, recovery;
    bool passwordChange = false;
    client::RemoteOutcome remote = client::RemoteOutcome::NotSent;
    client::LocalState local = client::LocalState::Unchanged;
    std::optional<client::ResponseMeta> confirmed;
    client::ClientError outputFailure() const;
private:
    bool json_;
    std::optional<std::string> profile_;
};
}  // namespace anof::app
