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


Result<QuotaState> reserveQuota(const QuotaState& state, QuotaDelta delta) {
    auto bytes = charge(state.limit.bytes, state.reserved.bytes, state.used.bytes, delta.bytes);
    if (!bytes) return bytes.error();
    auto nodes = charge(state.limit.nodes, state.reserved.nodes, state.used.nodes, delta.nodes);
    if (!nodes) return nodes.error();
    return QuotaState{state.limit, state.used, {bytes.value(), nodes.value()}};
}
Result<QuotaState> releaseQuotaReservation(const QuotaState& state, QuotaDelta delta) {
    if (!state.used.bytes.add(state.reserved.bytes.value()) || !state.used.nodes.add(state.reserved.nodes.value()) ||
        delta.bytes.value() > state.reserved.bytes.value() || delta.nodes.value() > state.reserved.nodes.value())
        return ErrorCode::IntegrityError;
    return QuotaState{state.limit, state.used,
        {ByteCount::from(state.reserved.bytes.value() - delta.bytes.value()).value(),
         NodeCount::from(state.reserved.nodes.value() - delta.nodes.value()).value()}};
}
Result<QuotaState> commitQuotaReservation(const QuotaState& state, QuotaDelta delta) {
    auto released = releaseQuotaReservation(state, delta);
    if (!released) return released.error();
    // 已预留占用转 used，总占用不增长；非法持久化逻辑额度不能通过发布洗掉。
    if (state.used.bytes.value() > state.limit.bytes.value() ||
        state.reserved.bytes.value() > state.limit.bytes.value() - state.used.bytes.value() ||
        state.used.nodes.value() > state.limit.nodes.value() ||
        state.reserved.nodes.value() > state.limit.nodes.value() - state.used.nodes.value()) return ErrorCode::IntegrityError;
    auto bytes = state.used.bytes.add(delta.bytes.value());
    auto nodes = state.used.nodes.add(delta.nodes.value());
    if (!bytes || !nodes) return ErrorCode::IntegrityError;
    released.value().used = {bytes.value(), nodes.value()};
    return released;
}
}  // namespace anof
