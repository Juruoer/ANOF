#include <anof/infrastructure/persistence/sqlite/SqliteIdentityAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <type_traits>
#include <optional>
#include <string>

namespace anof::infrastructure::sqlite {
Result<void> SqliteIdentityAuditRepository::append(const IdentityEvent& event) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    const char* action = nullptr;
    std::optional<std::string> actor, subject, invite, instance, space, user, scope;
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, InstanceInitialized>) {
            action = "instance_initialized";
            subject = value.administrator.toString();
            instance = value.instance.toString();
        } else if constexpr (std::is_same_v<T, InviteCreated> || std::is_same_v<T, InviteRevoked>) {
            action = std::is_same_v<T, InviteCreated> ? "invite_created" : "invite_revoked";
            actor = value.actor.toString();
            invite = value.invite.toString();
        } else if constexpr (std::is_same_v<T, UserRegistered>) {
            action = "user_registered";
            subject = value.subject.toString();
            invite = value.invite.toString();
        } else if constexpr (std::is_same_v<T, UserEnabled> || std::is_same_v<T, UserDisabled>) {
            action = std::is_same_v<T, UserEnabled> ? "user_enabled" : "user_disabled";
            actor = value.actor.toString();
            subject = value.subject.toString();
        } else if constexpr (std::is_same_v<T, AdministratorPasswordReset>) {
            action = "administrator_password_reset";
            subject = value.subject.toString();
        } else if constexpr (std::is_same_v<T, QuotaChanged>) {
            action = "quota_changed";
            actor = value.actor.toString();
            std::visit([&](const auto& q) {
                space = q.spaceId.toString();
                if constexpr (std::is_same_v<std::decay_t<decltype(q)>, PublicContributionQuota>) {
                    scope = "contribution";
                    user = q.userId.toString();
                } else {
                    scope = "space";
                }
            }, value.scope);
        } else {
            if constexpr (std::is_same_v<T, SessionCreated>) action = "session_created";
            else if constexpr (std::is_same_v<T, SessionRevoked>) action = "session_revoked";
            else if constexpr (std::is_same_v<T, PasswordChanged>) action = "password_changed";
            else if constexpr (std::is_same_v<T, DisplayNameChanged>) action = "display_name_changed";
            actor = value.subject.toString();
            subject = value.subject.toString();
        }
    }, event.action);
    auto parameter = [](const std::optional<std::string>& value) -> detail::Parameter {
        return value ? detail::Parameter(*value) : detail::Parameter(nullptr);
    };
    return detail::one(context_, detail::write(context_, "INSERT INTO identity_audit_events(actor_id,subject_user_id,invite_id,instance_id,space_id,user_id,quota_scope,request_id,action,occurred_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
    {
        parameter(actor), parameter(subject), parameter(invite), parameter(instance), parameter(space), parameter(user), parameter(scope), event.requestId.toString(), action, event.occurredAt.time_since_epoch().count() }));
}
}  // namespace anof::infrastructure::sqlite
