#pragma once

// Bootstrap e apply de réplica (Fases 14C/14D).

#include "modb/error.hpp"
#include "modb/net/replication_protocol.hpp"
#include "modb/object/database.hpp"
#include "modb/tx/wal.hpp"

#include <filesystem>
#include <span>
#include <vector>

namespace modb::repl {

// Copia o arquivo de dados do primary sob barreira do escritor e devolve o
// manifesto (cut_lsn = checkpoint atual).
struct BootstrapSnapshot {
    net::BootstrapBegin begin{};
    std::filesystem::path temp_copy{};
};

[[nodiscard]] Result<BootstrapSnapshot> create_bootstrap_snapshot(
    object::Database& primary, const std::filesystem::path& temp_dir);

// Grava chunks no destino e renomeia atomicamente para `follower_path`.
[[nodiscard]] Result<void> install_bootstrap_snapshot(
    const BootstrapSnapshot& snapshot, const std::filesystem::path& follower_path);

// Aplica registros WAL (page_image de txs com commit) de forma idempotente.
// Ignora commit_lsn <= applied_lsn; exige continuidade (sem gap).
[[nodiscard]] Result<std::uint64_t> apply_wal_records(
    storage::PageFile& file, std::span<const tx::WalRecord> records,
    std::uint64_t applied_lsn);

// Monta um WalFrame a partir do WAL durável do primary (from_lsn inclusive).
[[nodiscard]] Result<net::WalFrame> build_wal_frame(const std::filesystem::path& wal_path,
                                                    std::uint64_t from_lsn,
                                                    std::uint64_t oldest_available_lsn);

} // namespace modb::repl
