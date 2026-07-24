#pragma once

// Modo de armazenamento do primary (Fase 15 / ADR-017).

#include "modb/error.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace modb::object {

enum class PrimaryStorage : std::uint8_t {
    full = 0,     // dados + WAL (default, Fase 14)
    wal_only = 1, // só WAL + controle; dados nas réplicas
};

// Quando o primary `wal_only` confirma o commit ao cliente.
enum class CommitAckPolicy : std::uint8_t {
    local_wal = 0,          // após fsync do WAL local
    await_one_replica = 1,  // default wal_only: ACK de ≥1 réplica de dados
};

struct DatabaseOptions {
    PrimaryStorage primary_storage{PrimaryStorage::full};
    CommitAckPolicy commit_ack{CommitAckPolicy::local_wal};
    std::chrono::milliseconds commit_ack_timeout{std::chrono::seconds{5}};
};

[[nodiscard]] inline constexpr std::string_view to_string(PrimaryStorage storage) noexcept {
    switch (storage) {
    case PrimaryStorage::full:
        return "full";
    case PrimaryStorage::wal_only:
        return "wal_only";
    }
    return "full";
}

[[nodiscard]] Result<PrimaryStorage> parse_primary_storage(std::string_view text);

} // namespace modb::object
