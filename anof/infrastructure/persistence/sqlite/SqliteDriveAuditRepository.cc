#include <anof/infrastructure/persistence/sqlite/SqliteDriveAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
namespace anof::infrastructure::sqlite {
Result<void> SqliteDriveAuditRepository::append(const DriveEvent& e) {
    std::string first, second, third;
    std::visit([&](const auto& action) {
        using T = std::decay_t<decltype(action)>;
        if constexpr (std::is_same_v<T, FileCopied>) {
            first = action.source.toString();
            second = action.result.toString();
        } else {
            first = action.id.toString();
            if constexpr (std::is_same_v<T, NodeMoved>) {
                second = action.from.toString();
                third = action.to.toString();
            } else if constexpr (std::is_same_v<T, TreeTrashed> || std::is_same_v<T, TreeRestored> || std::is_same_v<T, TreePurged>)
                second = action.root.toString();
        }
    },
               e.action);
    const auto* actor = std::get_if<UserId>(&e.actor);
    if (!actor && e.action.index() < 5) return context_.fail(ErrorCode::InvalidArgument);
    return detail::one(context_, detail::write(context_, "INSERT INTO drive_audit_events(request_id,observed_at,actor_id,maintenance,kind,first_id,second_id,third_id) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)",
                                               { e.requestId.toString(), e.observedAt.time_since_epoch().count(), actor ? detail::Parameter(actor->toString()) : detail::Parameter(nullptr), std::int64_t(!actor), std::int64_t(e.action.index()), first, second.empty() ? detail::Parameter(nullptr) : detail::Parameter(second), third.empty() ? detail::Parameter(nullptr) : detail::Parameter(third) }));
}
}  // namespace anof::infrastructure::sqlite
