#include "modb/repl/wal_downloader.hpp"

#include "modb/object/database.hpp"
#include "modb/repl/replication.hpp"
#include "modb/storage/native_file.hpp"
#include "modb/tx/wal.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <system_error>

namespace modb::repl {
namespace {

Result<void> ensure_directory(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return std::unexpected(
            Error{ErrorCode::io_error, "could not create WAL spool directory: " + ec.message()});
    }
    return {};
}

Result<void> validate_downloaded_segment(const WalSegmentManifest& segment,
                                         const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::replica_download_failed,
                                     "could not stat downloaded WAL segment: " + ec.message()});
    }
    if (size != segment.size_bytes) {
        return std::unexpected(
            Error{ErrorCode::manifest_hash_mismatch, "downloaded WAL segment size mismatch"});
    }
    auto hash = hash_file_for_manifest(path);
    if (!hash) {
        return std::unexpected(hash.error());
    }
    if (*hash != segment.hash) {
        return std::unexpected(
            Error{ErrorCode::manifest_hash_mismatch, "downloaded WAL segment hash mismatch"});
    }
    return {};
}

Result<object::Database> open_or_create_replica(const ReplicaCatchupOptions& options) {
    std::error_code ec;
    if (std::filesystem::exists(options.replica_path, ec)) {
        auto opened = object::Database::open(options.replica_path);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        if (opened->database_uuid() != options.database_uuid) {
            return std::unexpected(
                Error{ErrorCode::database_uuid_mismatch, "replica database UUID mismatch"});
        }
        if (opened->timeline_id() != options.timeline) {
            return std::unexpected(Error{ErrorCode::timeline_mismatch, "replica timeline mismatch"});
        }
        return *std::move(opened);
    }
    if (ec) {
        return std::unexpected(
            Error{ErrorCode::io_error, "could not stat replica file: " + ec.message()});
    }
    auto created = object::Database::create(options.replica_path);
    if (!created) {
        return std::unexpected(created.error());
    }
    if (auto identity = created->set_replication_identity(options.database_uuid, options.timeline);
        !identity) {
        return std::unexpected(identity.error());
    }
    return *std::move(created);
}

} // namespace

Result<WalDownloadResult> download_wal_segments(const WalManifest& manifest,
                                                const WalDownloadOptions& options) {
    if (auto dir = ensure_directory(options.spool_dir); !dir) {
        return std::unexpected(dir.error());
    }
    WalDownloadResult result;
    result.first_lsn = manifest.first_lsn;
    result.last_lsn = manifest.last_lsn;
    for (const auto& segment : manifest.segments) {
        if (segment.source_path.empty()) {
            return std::unexpected(
                Error{ErrorCode::replica_download_failed, "WAL segment has no source path"});
        }
        const auto final_path = options.spool_dir / segment.segment_id;
        std::error_code ec;
        if (options.resume && std::filesystem::exists(final_path, ec)) {
            if (auto valid = validate_downloaded_segment(segment, final_path); valid) {
                result.segment_paths.push_back(final_path);
                continue;
            }
        }
        const auto tmp_path = std::filesystem::path{final_path.string() + ".tmp"};
        std::filesystem::remove(tmp_path, ec);
        ec.clear();
        std::filesystem::copy_file(segment.source_path, tmp_path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::replica_download_failed,
                                         "could not download WAL segment: " + ec.message()});
        }
        if (auto valid = validate_downloaded_segment(segment, tmp_path); !valid) {
            return std::unexpected(valid.error());
        }
        auto synced = storage::NativeFile::open(tmp_path, storage::NativeFile::Mode::open_existing);
        if (!synced) {
            return std::unexpected(synced.error());
        }
        if (auto sync = synced->sync(); !sync) {
            return std::unexpected(sync.error());
        }
        if (auto close = synced->close(); !close) {
            return std::unexpected(close.error());
        }
        std::filesystem::rename(tmp_path, final_path, ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::replica_download_failed,
                                         "could not publish WAL segment: " + ec.message()});
        }
        result.segment_paths.push_back(final_path);
    }
    return result;
}

Result<ReplicaCatchupResult> catch_up_replica_from_wal(const ReplicaCatchupOptions& options) {
    auto existing = read_catchup_metadata(options.replica_path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    auto transition = validate_catchup_transition(existing->state, CatchupState::catching_up);
    if (!transition) {
        return std::unexpected(transition.error());
    }

    auto replica = open_or_create_replica(options);
    if (!replica) {
        CatchupMetadata failed{CatchupState::failed, existing->applied_lsn, existing->target_lsn,
                               replica.error().message};
        (void)write_catchup_metadata(options.replica_path, failed);
        return std::unexpected(replica.error());
    }

    const std::uint64_t discovered_lsn =
        std::max(replica->follower_ack_lsn(), replica->checkpoint_lsn());
    const std::uint64_t applied_before =
        options.applied_lsn_is_explicit ? options.applied_lsn
                                        : std::max(existing->applied_lsn, discovered_lsn);

    auto manifest = build_wal_manifest(options.wal_source, options.database_uuid, options.timeline,
                                       options.oldest_available_lsn);
    if (!manifest) {
        CatchupMetadata failed{CatchupState::failed, applied_before, applied_before,
                               manifest.error().message};
        (void)write_catchup_metadata(options.replica_path, failed);
        return std::unexpected(manifest.error());
    }
    if (auto valid = validate_wal_manifest(*manifest, options.database_uuid, options.timeline,
                                           applied_before);
        !valid) {
        const auto state = valid.error().code == ErrorCode::bootstrap_required
                               ? CatchupState::requires_bootstrap
                               : CatchupState::failed;
        (void)write_catchup_metadata(
            options.replica_path,
            CatchupMetadata{state, applied_before, manifest->last_lsn, valid.error().message});
        return std::unexpected(valid.error());
    }

    std::uint64_t apply_base_lsn = applied_before;
    if (applied_before == 0 && !manifest->segments.empty() && manifest->oldest_available_lsn <= 1 &&
        manifest->first_lsn > 1) {
        apply_base_lsn = manifest->first_lsn - 1;
    }

    if (auto write = write_catchup_metadata(
            options.replica_path,
            CatchupMetadata{CatchupState::catching_up, apply_base_lsn, manifest->last_lsn, {}});
        !write) {
        return std::unexpected(write.error());
    }

    auto downloaded = download_wal_segments(*manifest, WalDownloadOptions{options.spool_dir, true});
    if (!downloaded) {
        (void)write_catchup_metadata(options.replica_path,
                                     CatchupMetadata{CatchupState::failed, apply_base_lsn,
                                                     manifest->last_lsn,
                                                     downloaded.error().message});
        return std::unexpected(downloaded.error());
    }

    if (auto ro = replica->set_read_only_replica(true); !ro) {
        return std::unexpected(ro.error());
    }

    std::uint64_t applied = apply_base_lsn;
    for (const auto& segment_path : downloaded->segment_paths) {
        auto records = tx::Wal::read_for_replication(segment_path, applied + 1);
        if (!records) {
            (void)write_catchup_metadata(
                options.replica_path,
                CatchupMetadata{CatchupState::failed, applied, manifest->last_lsn,
                                records.error().message});
            return std::unexpected(records.error());
        }
        auto next = apply_wal_records(replica->page_file(), *records, applied);
        if (!next) {
            const auto state = next.error().code == ErrorCode::replication_gap
                                   ? CatchupState::requires_bootstrap
                                   : CatchupState::failed;
            (void)write_catchup_metadata(
                options.replica_path,
                CatchupMetadata{state, applied, manifest->last_lsn, next.error().message});
            return std::unexpected(next.error());
        }
        applied = *next;
    }
    if (auto ack = replica->set_follower_ack_lsn(applied); !ack) {
        return std::unexpected(ack.error());
    }
    const auto state = applied >= manifest->last_lsn ? CatchupState::up_to_date
                                                     : CatchupState::catching_up;
    if (auto write = write_catchup_metadata(
            options.replica_path, CatchupMetadata{state, applied, manifest->last_lsn, {}});
        !write) {
        return std::unexpected(write.error());
    }
    return ReplicaCatchupResult{state, applied, manifest->last_lsn, *manifest};
}

} // namespace modb::repl
