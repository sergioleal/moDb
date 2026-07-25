#include "modb/repl/wal_manifest.hpp"

#include "modb/storage/native_file.hpp"
#include "modb/tx/wal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <span>
#include <system_error>

namespace modb::repl {
namespace {

Result<std::uint64_t> parse_u64(std::string_view text, std::string_view field) {
    std::uint64_t value{};
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::unexpected(Error{ErrorCode::invalid_encoding,
                                     std::string{"invalid catch-up "} + std::string{field}});
    }
    return value;
}

Result<std::string> read_value(std::string_view line, std::string_view key) {
    const auto pos = line.find('=');
    if (pos == std::string_view::npos || line.substr(0, pos) != key) {
        return std::unexpected(Error{ErrorCode::invalid_encoding,
                                     std::string{"expected catch-up field "} + std::string{key}});
    }
    return std::string{line.substr(pos + 1)};
}

Result<std::vector<std::byte>> read_all_bytes(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::io_error, "could not stat file: " + ec.message()});
    }
    auto file = storage::NativeFile::open(path, storage::NativeFile::Mode::open_existing);
    if (!file) {
        return std::unexpected(file.error());
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        if (auto read = file->read_at(0, bytes); !read) {
            return std::unexpected(read.error());
        }
    }
    return bytes;
}

std::string fnv1a64_hex(std::span<const std::byte> bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

} // namespace

std::string_view to_string(CatchupState state) noexcept {
    switch (state) {
    case CatchupState::empty:
        return "empty";
    case CatchupState::seeding:
        return "seeding";
    case CatchupState::catching_up:
        return "catching_up";
    case CatchupState::up_to_date:
        return "up_to_date";
    case CatchupState::requires_bootstrap:
        return "requires_bootstrap";
    case CatchupState::failed:
        return "failed";
    }
    return "failed";
}

Result<CatchupState> parse_catchup_state(std::string_view text) {
    if (text == "empty") {
        return CatchupState::empty;
    }
    if (text == "seeding") {
        return CatchupState::seeding;
    }
    if (text == "catching_up") {
        return CatchupState::catching_up;
    }
    if (text == "up_to_date") {
        return CatchupState::up_to_date;
    }
    if (text == "requires_bootstrap") {
        return CatchupState::requires_bootstrap;
    }
    if (text == "failed") {
        return CatchupState::failed;
    }
    return std::unexpected(
        Error{ErrorCode::invalid_replica_state, "unknown catch-up state"});
}

Result<void> validate_catchup_transition(CatchupState from, CatchupState to) {
    if (from == to) {
        return {};
    }
    const bool ok =
        (from == CatchupState::empty &&
         (to == CatchupState::seeding || to == CatchupState::catching_up ||
          to == CatchupState::requires_bootstrap || to == CatchupState::failed)) ||
        (from == CatchupState::seeding &&
         (to == CatchupState::catching_up || to == CatchupState::up_to_date ||
          to == CatchupState::requires_bootstrap || to == CatchupState::failed)) ||
        (from == CatchupState::catching_up &&
         (to == CatchupState::up_to_date || to == CatchupState::requires_bootstrap ||
          to == CatchupState::failed)) ||
        (from == CatchupState::up_to_date && to == CatchupState::catching_up) ||
        (from == CatchupState::failed &&
         (to == CatchupState::catching_up || to == CatchupState::requires_bootstrap)) ||
        (from == CatchupState::requires_bootstrap &&
         (to == CatchupState::seeding || to == CatchupState::catching_up));
    if (!ok) {
        return std::unexpected(Error{ErrorCode::invalid_replica_state,
                                     "invalid catch-up state transition"});
    }
    return {};
}

std::filesystem::path catchup_metadata_path(const std::filesystem::path& replica_path) {
    return std::filesystem::path{replica_path.string() + ".catchup"};
}

Result<CatchupMetadata> read_catchup_metadata(const std::filesystem::path& replica_path) {
    const auto path = catchup_metadata_path(replica_path);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return CatchupMetadata{};
    }
    if (ec) {
        return std::unexpected(Error{ErrorCode::io_error, "could not stat catch-up metadata: " +
                                                            ec.message()});
    }
    std::ifstream in(path);
    if (!in) {
        return std::unexpected(
            Error{ErrorCode::io_error, "could not open catch-up metadata"});
    }
    std::array<std::string, 4> lines{};
    for (auto& line : lines) {
        if (!std::getline(in, line)) {
            return std::unexpected(Error{ErrorCode::invalid_encoding,
                                         "truncated catch-up metadata"});
        }
    }
    auto state_text = read_value(lines[0], "state");
    if (!state_text) {
        return std::unexpected(state_text.error());
    }
    auto state = parse_catchup_state(*state_text);
    if (!state) {
        return std::unexpected(state.error());
    }
    auto applied_text = read_value(lines[1], "applied_lsn");
    auto target_text = read_value(lines[2], "target_lsn");
    auto error_text = read_value(lines[3], "last_error");
    if (!applied_text) {
        return std::unexpected(applied_text.error());
    }
    if (!target_text) {
        return std::unexpected(target_text.error());
    }
    if (!error_text) {
        return std::unexpected(error_text.error());
    }
    auto applied = parse_u64(*applied_text, "applied_lsn");
    auto target = parse_u64(*target_text, "target_lsn");
    if (!applied) {
        return std::unexpected(applied.error());
    }
    if (!target) {
        return std::unexpected(target.error());
    }
    return CatchupMetadata{*state, *applied, *target, *error_text};
}

Result<void> write_catchup_metadata(const std::filesystem::path& replica_path,
                                    const CatchupMetadata& metadata) {
    const auto path = catchup_metadata_path(replica_path);
    const auto tmp = std::filesystem::path{path.string() + ".tmp"};
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            return std::unexpected(
                Error{ErrorCode::io_error, "could not write catch-up metadata"});
        }
        out << "state=" << to_string(metadata.state) << '\n'
            << "applied_lsn=" << metadata.applied_lsn << '\n'
            << "target_lsn=" << metadata.target_lsn << '\n'
            << "last_error=" << metadata.last_error << '\n';
    }
    auto synced = storage::NativeFile::open(tmp, storage::NativeFile::Mode::open_existing);
    if (!synced) {
        return std::unexpected(synced.error());
    }
    if (auto sync = synced->sync(); !sync) {
        return std::unexpected(sync.error());
    }
    if (auto closed = synced->close(); !closed) {
        return std::unexpected(closed.error());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::io_error, "could not publish catch-up metadata: " +
                                                        ec.message()});
    }
    return {};
}

Result<std::string> hash_file_for_manifest(const std::filesystem::path& path) {
    auto bytes = read_all_bytes(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return fnv1a64_hex(*bytes);
}

Result<WalManifest> build_wal_manifest(const std::filesystem::path& wal_path,
                                       object::DatabaseUuid uuid,
                                       object::TimelineId timeline,
                                       std::uint64_t oldest_available_lsn) {
    auto records = tx::Wal::read_for_replication(wal_path, oldest_available_lsn);
    if (!records) {
        return std::unexpected(records.error());
    }
    WalManifest manifest;
    manifest.database_uuid = uuid;
    manifest.timeline = timeline;
    manifest.oldest_available_lsn = oldest_available_lsn;
    if (records->empty()) {
        manifest.first_lsn = oldest_available_lsn;
        manifest.last_lsn = oldest_available_lsn == 0 ? 0 : oldest_available_lsn - 1;
        return manifest;
    }
    for (std::size_t i = 1; i < records->size(); ++i) {
        if ((*records)[i].lsn != (*records)[i - 1].lsn + 1) {
            return std::unexpected(
                Error{ErrorCode::replication_gap, "WAL manifest found non-contiguous LSNs"});
        }
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(wal_path, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::io_error, "could not stat WAL: " + ec.message()});
    }
    auto hash = hash_file_for_manifest(wal_path);
    if (!hash) {
        return std::unexpected(hash.error());
    }
    manifest.first_lsn = records->front().lsn;
    manifest.last_lsn = records->back().lsn;
    manifest.segments.push_back(WalSegmentManifest{
        wal_path.filename().string(), manifest.first_lsn, manifest.last_lsn,
        static_cast<std::uint64_t>(size), *hash, wal_path});
    return manifest;
}

Result<void> validate_wal_manifest(const WalManifest& manifest,
                                   object::DatabaseUuid expected_uuid,
                                   object::TimelineId expected_timeline,
                                   std::uint64_t applied_lsn) {
    if (manifest.database_uuid != expected_uuid) {
        return std::unexpected(
            Error{ErrorCode::database_uuid_mismatch, "WAL manifest database UUID mismatch"});
    }
    if (manifest.timeline != expected_timeline) {
        return std::unexpected(
            Error{ErrorCode::timeline_mismatch, "WAL manifest timeline mismatch"});
    }
    const auto next_needed = applied_lsn == 0 ? 1 : applied_lsn + 1;
    if (manifest.segments.empty()) {
        if (next_needed < manifest.oldest_available_lsn) {
            return std::unexpected(Error{ErrorCode::bootstrap_required,
                                         "WAL manifest does not cover replica gap"});
        }
        return {};
    }
    std::uint64_t expected = manifest.segments.front().first_lsn;
    for (const auto& segment : manifest.segments) {
        if (segment.segment_id.empty() || segment.hash.empty() || segment.first_lsn == 0 ||
            segment.last_lsn < segment.first_lsn || segment.first_lsn != expected) {
            return std::unexpected(
                Error{ErrorCode::invalid_encoding, "invalid WAL manifest segment"});
        }
        expected = segment.last_lsn + 1;
    }
    if (manifest.first_lsn != manifest.segments.front().first_lsn ||
        manifest.last_lsn != manifest.segments.back().last_lsn) {
        return std::unexpected(
            Error{ErrorCode::invalid_encoding, "WAL manifest range disagrees with segments"});
    }
    const bool empty_replica_from_origin =
        applied_lsn == 0 && manifest.oldest_available_lsn <= 1 && manifest.first_lsn > 1;
    if (!empty_replica_from_origin &&
        (next_needed < manifest.first_lsn || next_needed < manifest.oldest_available_lsn)) {
        return std::unexpected(
            Error{ErrorCode::bootstrap_required, "WAL manifest does not cover replica gap"});
    }
    return {};
}

} // namespace modb::repl
