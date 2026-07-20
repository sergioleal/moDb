#include "modb/repl/replication.hpp"

#include "modb/storage/endian.hpp"
#include "modb/storage/page.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace modb::repl {
namespace {

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
            }
            result[index] = value;
        }
        return result;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc = table[(crc ^ std::to_integer<std::uint8_t>(byte)) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

Result<void> copy_file_bytes(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::io_error, "bootstrap copy failed: " + ec.message()});
    }
    return {};
}

} // namespace

Result<BootstrapSnapshot> create_bootstrap_snapshot(object::Database& primary,
                                                    const std::filesystem::path& temp_dir) {
    // Barreira: nenhuma outra tx; usamos begin+rollback como trava do escritor.
    auto barrier = primary.begin();
    if (!barrier) {
        return std::unexpected(barrier.error());
    }
    BootstrapSnapshot snap;
    snap.begin.page_size = static_cast<std::uint32_t>(storage::page_size);
    snap.begin.cut_lsn = primary.checkpoint_lsn();
    snap.begin.epoch = primary.epoch();
    snap.begin.baseline =
        primary.current_baseline() ? primary.current_baseline()->id().value : 0;
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);
    snap.temp_copy = temp_dir / ("bootstrap-" + std::to_string(snap.begin.cut_lsn) + ".modb");
    if (auto copied = copy_file_bytes(primary.data_path(), snap.temp_copy); !copied) {
        (void)barrier->rollback();
        return std::unexpected(copied.error());
    }
    snap.begin.size_bytes = std::filesystem::file_size(snap.temp_copy, ec);
    if (ec) {
        (void)barrier->rollback();
        return std::unexpected(Error{ErrorCode::io_error, ec.message()});
    }
    // CRC do arquivo completo (pode ser grande; OK para testes/MVP).
    std::ifstream in(snap.temp_copy, std::ios::binary);
    std::vector<std::byte> buf(static_cast<std::size_t>(snap.begin.size_bytes));
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    snap.begin.content_crc = crc32(buf);
    if (auto rolled = barrier->rollback(); !rolled) {
        return std::unexpected(rolled.error());
    }
    return snap;
}

Result<void> install_bootstrap_snapshot(const BootstrapSnapshot& snapshot,
                                        const std::filesystem::path& follower_path) {
    std::error_code ec;
    const auto tmp = follower_path.string() + ".tmp";
    std::filesystem::copy_file(snapshot.temp_copy, tmp,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::io_error, "install copy failed: " + ec.message()});
    }
    std::filesystem::rename(tmp, follower_path, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::io_error, "install rename failed: " + ec.message()});
    }
    return {};
}

Result<std::uint64_t> apply_wal_records(storage::PageFile& file,
                                        std::span<const tx::WalRecord> records,
                                        std::uint64_t applied_lsn) {
    std::unordered_map<std::uint64_t, std::uint64_t> committed;
    for (const auto& record : records) {
        if (record.type == tx::WalRecordType::commit) {
            committed[record.tx_id] = record.commit_lsn();
        }
    }

    std::uint64_t max_applied = applied_lsn;
    std::uint64_t expected = applied_lsn == 0 ? 1 : applied_lsn + 1;

    // Verifica continuidade dos LSNs no lote (sem pular gap).
    for (const auto& record : records) {
        if (record.lsn < expected) {
            continue; // duplicata / já aplicado
        }
        if (record.lsn > expected) {
            return std::unexpected(Error{ErrorCode::replication_gap,
                                         "WAL gap: expected " + std::to_string(expected) +
                                             " got " + std::to_string(record.lsn)});
        }
        ++expected;
    }

    for (const auto& record : records) {
        if (record.type != tx::WalRecordType::page_image) {
            if (record.type == tx::WalRecordType::commit) {
                const auto clsn = record.commit_lsn();
                if (clsn > applied_lsn && clsn > max_applied) {
                    max_applied = clsn;
                }
            }
            continue;
        }
        const auto it = committed.find(record.tx_id);
        if (it == committed.end() || it->second <= applied_lsn) {
            continue;
        }
        if (record.payload.size() != storage::page_size) {
            return std::unexpected(
                Error{ErrorCode::corrupt_file, "replicated page image has wrong size"});
        }
        storage::Page page;
        std::copy(record.payload.begin(), record.payload.end(), page.bytes().begin());
        if (auto written = file.write_recovered_page(storage::PageId{record.page_id}, page);
            !written) {
            return std::unexpected(written.error());
        }
        if (it->second > max_applied) {
            max_applied = it->second;
        }
    }
    if (max_applied > applied_lsn) {
        if (auto flushed = file.flush(); !flushed) {
            return std::unexpected(flushed.error());
        }
    }
    return max_applied;
}

Result<net::WalFrame> build_wal_frame(const std::filesystem::path& wal_path,
                                      std::uint64_t from_lsn,
                                      std::uint64_t oldest_available_lsn) {
    if (from_lsn < oldest_available_lsn) {
        return std::unexpected(Error{ErrorCode::replication_gap, "from_lsn below retention"});
    }
    auto records = tx::Wal::read_for_replication(wal_path, from_lsn);
    if (!records) {
        return std::unexpected(records.error());
    }
    if (records->empty()) {
        net::WalFrame empty;
        empty.first_lsn = from_lsn;
        empty.last_lsn = from_lsn;
        return empty;
    }
    // Empacota os bytes brutos re-lendo o arquivo seria ideal; MVP: re-serializa
    // via encode não disponível — usa read_all path e concatena payloads mínimos
    // para o teste de streaming, enviando registros via apply direto no teste.
    // Aqui devolvemos metadados + serialização simplificada (lista de LSNs no crc).
    net::WalFrame frame;
    frame.first_lsn = records->front().lsn;
    frame.last_lsn = records->back().lsn;
    storage::BinaryWriter w;
    w.write_u32(static_cast<std::uint32_t>(records->size()));
    for (const auto& r : *records) {
        w.write_u64(r.lsn);
        w.write_u64(r.tx_id);
        w.write_u8(static_cast<std::uint8_t>(r.type));
        w.write_u64(r.page_id);
        w.write_u32(static_cast<std::uint32_t>(r.payload.size()));
        w.write_bytes(r.payload);
    }
    const auto packed = w.bytes();
    frame.records.assign(packed.begin(), packed.end());
    frame.crc = crc32(frame.records);
    return frame;
}

} // namespace modb::repl
