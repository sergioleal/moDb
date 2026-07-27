#include "modb/diag/stage_profile.hpp"

#if defined(MODB_ENABLE_STAGE_PROFILING)
#include <atomic>
#endif

namespace modb::diag {

std::string_view stage_name(Stage stage) noexcept {
    switch (stage) {
    case Stage::heap_candidate_scan:
        return "heap_candidate_scan";
    case Stage::heap_candidate_try:
        return "heap_candidate_try";
    case Stage::heap_page_write:
        return "heap_page_write";
    case Stage::persist_root:
        return "persist_root";
    case Stage::wal_append:
        return "wal_append";
    case Stage::wal_sync:
        return "wal_sync";
    case Stage::object_encode:
        return "object_encode";
    case Stage::count_:
        break;
    }
    return "unknown";
}

#if defined(MODB_ENABLE_STAGE_PROFILING)

namespace {

struct AtomicTotals {
    std::atomic<std::uint64_t> elapsed_ns{0};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> units{0};
};

// Duração estática: os contadores precisam sobreviver a qualquer ordem de
// destruição de objetos do motor que ainda registre algo no caminho de saída.
AtomicTotals& totals_for(Stage stage) noexcept {
    static std::array<AtomicTotals, stage_count> totals;
    return totals[static_cast<std::size_t>(stage)];
}

} // namespace

void stage_record(Stage stage, std::uint64_t elapsed_ns, std::uint64_t units) noexcept {
    auto& t = totals_for(stage);
    t.elapsed_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    t.calls.fetch_add(1, std::memory_order_relaxed);
    if (units != 0) {
        t.units.fetch_add(units, std::memory_order_relaxed);
    }
}

void stage_add_units(Stage stage, std::uint64_t units) noexcept {
    totals_for(stage).units.fetch_add(units, std::memory_order_relaxed);
}

void stage_reset() noexcept {
    for (std::size_t i = 0; i < stage_count; ++i) {
        auto& t = totals_for(static_cast<Stage>(i));
        t.elapsed_ns.store(0, std::memory_order_relaxed);
        t.calls.store(0, std::memory_order_relaxed);
        t.units.store(0, std::memory_order_relaxed);
    }
}

StageSnapshot stage_snapshot() noexcept {
    StageSnapshot snapshot;
    for (std::size_t i = 0; i < stage_count; ++i) {
        auto& t = totals_for(static_cast<Stage>(i));
        snapshot[i].elapsed_ns = t.elapsed_ns.load(std::memory_order_relaxed);
        snapshot[i].calls = t.calls.load(std::memory_order_relaxed);
        snapshot[i].units = t.units.load(std::memory_order_relaxed);
    }
    return snapshot;
}

#endif

} // namespace modb::diag
