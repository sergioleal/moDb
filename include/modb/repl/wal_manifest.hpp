#pragma once

#include "modb/error.hpp"
#include "modb/object/database.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace modb::repl {

enum class CatchupState : std::uint8_t {
    empty = 0,
    seeding = 1,
    catching_up = 2,
    up_to_date = 3,
    requires_bootstrap = 4,
    failed = 5,
};

struct CatchupMetadata {
    CatchupState state{CatchupState::empty};
    std::uint64_t applied_lsn{};
    std::uint64_t target_lsn{};
    std::string last_error;

    friend bool operator==(const CatchupMetadata&, const CatchupMetadata&) = default;
};

struct WalSegmentManifest {
    std::string segment_id;
    std::uint64_t first_lsn{};
    std::uint64_t last_lsn{};
    std::uint64_t size_bytes{};
    std::string hash;
    std::filesystem::path source_path;

    friend bool operator==(const WalSegmentManifest&, const WalSegmentManifest&) = default;
};

struct WalManifest {
    object::DatabaseUuid database_uuid{};
    object::TimelineId timeline{};
    std::uint64_t first_lsn{};
    std::uint64_t last_lsn{};
    std::uint64_t oldest_available_lsn{1};
    std::vector<WalSegmentManifest> segments;

    friend bool operator==(const WalManifest&, const WalManifest&) = default;
};

[[nodiscard]] std::string_view to_string(CatchupState state) noexcept;
[[nodiscard]] Result<CatchupState> parse_catchup_state(std::string_view text);
[[nodiscard]] Result<void> validate_catchup_transition(CatchupState from,
                                                       CatchupState to);

[[nodiscard]] std::filesystem::path catchup_metadata_path(
    const std::filesystem::path& replica_path);
[[nodiscard]] Result<CatchupMetadata> read_catchup_metadata(
    const std::filesystem::path& replica_path);
[[nodiscard]] Result<void> write_catchup_metadata(const std::filesystem::path& replica_path,
                                                  const CatchupMetadata& metadata);

[[nodiscard]] Result<WalManifest> build_wal_manifest(
    const std::filesystem::path& wal_path, object::DatabaseUuid uuid,
    object::TimelineId timeline, std::uint64_t oldest_available_lsn);
[[nodiscard]] Result<void> validate_wal_manifest(const WalManifest& manifest,
                                                 object::DatabaseUuid expected_uuid,
                                                 object::TimelineId expected_timeline,
                                                 std::uint64_t applied_lsn);

[[nodiscard]] Result<std::string> hash_file_for_manifest(const std::filesystem::path& path);

} // namespace modb::repl
