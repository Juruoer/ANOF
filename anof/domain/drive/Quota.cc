#include <anof/domain/drive/Quota.h>

namespace anof {
namespace {

template <typename T>
Result<T> charge(T limit, T used, T reserved, T delta) {
    auto occupied = used.add(reserved.value());
    if (!occupied) return occupied.error();
    if (occupied.value().value() > limit.value()) return ErrorCode::IntegrityError;
    auto total = occupied.value().add(delta.value());
    if (!total) return total.error();
    if (total.value().value() > limit.value()) return ErrorCode::QuotaExceeded;
    return used.add(delta.value());
}

}  // namespace

Result<QuotaState> chargeUsed(const QuotaState& state, QuotaDelta delta) {
    auto bytes = charge(state.limit.bytes, state.used.bytes, state.reserved.bytes, delta.bytes);
    if (!bytes) return bytes.error();
    auto nodes = charge(state.limit.nodes, state.used.nodes, state.reserved.nodes, delta.nodes);
    if (!nodes) return nodes.error();
    return QuotaState{ state.limit, { bytes.value(), nodes.value() }, state.reserved };
}

}  // namespace anof
