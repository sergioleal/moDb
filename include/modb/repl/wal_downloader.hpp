#pragma once

#include "modb/repl/wal_manifest.hpp"

#include <filesystem>
#include <vector>

namespace modb::repl {

struct WalDownloadOptions {
    std::filesystem::path spool_dir;
    bool resume{true};
};

struct WalDownloadResult {
    std::vector<std::filesystem::path> segment_paths;
    std::uint64_t first_lsn{};
    std::uint64_t last_lsn{};
};

struct ReplicaCatchupOptions {
    std::filesystem::path replica_path;
    std::filesystem::path wal_source;
    std::filesystem::path spool_dir;
    object::DatabaseUuid database_uuid{};
    object::TimelineId timeline{};
    std::uint64_t oldest_available_lsn{1};
    std::uint64_t applied_lsn{};
    bool applied_lsn_is_explicit{false};
};

struct ReplicaCatchupResult {
    CatchupState state{CatchupState::empty};
    std::uint64_t applied_lsn{};
    std::uint64_t target_lsn{};
    WalManifest manifest{};
};

[[nodiscard]] Result<WalDownloadResult> download_wal_segments(
    const WalManifest& manifest, const WalDownloadOptions& options);

[[nodiscard]] Result<ReplicaCatchupResult> catch_up_replica_from_wal(
    const ReplicaCatchupOptions& options);

} // namespace modb::repl
