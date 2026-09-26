// Importa a interface da recuperação.
#include "modb/tx/recovery.hpp"

// Importa o leitor do WAL.
#include "modb/tx/wal.hpp"
// Importa Page/page_size para reconstruir as imagens.
#include "modb/storage/page.hpp"
// Importa PageFile, alvo da reaplicação.
#include "modb/storage/page_file.hpp"

// Disponibiliza std::copy ao reconstruir a página.
#include <algorithm>
// Disponibiliza caminhos e a existência do arquivo.
#include <filesystem>
// Disponibiliza o conjunto de transações commitadas.
#include <unordered_map>
#include <unordered_set>

namespace modb::tx {

Result<RecoverResult> recover(storage::PageFile& file, const std::filesystem::path& wal_path,
                              std::uint64_t after_lsn, std::uint64_t start_offset) {
    RecoverResult result{};
    std::error_code exists_error;
    if (!std::filesystem::exists(wal_path, exists_error)) {
        return result;
    }

    // Leitura a partir do checkpoint, quando o offset gravado é confiável. Um
    // offset velho (WAL recriado, arquivo menor, meio de registro) cai na
    // leitura completa: o custo é só tempo, nunca correção.
    Result<std::vector<WalRecord>> records{std::vector<WalRecord>{}};
    bool bounded = false;
    std::error_code size_error;
    const auto wal_size = std::filesystem::file_size(wal_path, size_error);
    if (!size_error && start_offset > wal_header_size && start_offset <= wal_size) {
        auto tail = Wal::read_from_offset(wal_path, start_offset);
        if (tail && (start_offset == wal_size ||
                     (!tail->empty() && tail->front().lsn > after_lsn))) {
            records = std::move(tail);
            bounded = true;
        }
    }
    if (!bounded) {
        records = Wal::read_all(wal_path);
    }
    result.read_from_offset = bounded;
    result.last_commit_end = bounded ? start_offset : wal_header_size;
    if (!records) {
        return std::unexpected(Error{ErrorCode::wal_corrupt,
                                     "cannot recover unreadable WAL: " +
                                         records.error().message});
    }

    // tx_id → commit_lsn (v2) ou lsn do registro commit (v1).
    std::unordered_map<std::uint64_t, std::uint64_t> committed;
    for (const auto& record : *records) {
        if (record.type == WalRecordType::commit) {
            committed[record.tx_id] = record.commit_lsn();
            result.last_commit_end = record.end_offset;
        }
        result.max_lsn = record.lsn > result.max_lsn ? record.lsn : result.max_lsn;
        result.max_tx_id = record.tx_id > result.max_tx_id ? record.tx_id : result.max_tx_id;
    }

    for (const auto& record : *records) {
        if (record.type != WalRecordType::page_image) {
            continue;
        }
        const auto it = committed.find(record.tx_id);
        if (it == committed.end()) {
            continue;
        }
        if (it->second <= after_lsn) {
            continue;
        }
        if (record.payload.size() != storage::page_size) {
            return std::unexpected(
                Error{ErrorCode::corrupt_file, "WAL page image has the wrong size"});
        }
        storage::Page page;
        std::copy(record.payload.begin(), record.payload.end(), page.bytes().begin());
        if (auto written = file.write_recovered_page(storage::PageId{record.page_id}, page);
            !written) {
            return std::unexpected(written.error());
        }
        result.applied_any = true;
        if (it->second > result.max_commit_lsn) {
            result.max_commit_lsn = it->second;
        }
    }
    if (result.applied_any) {
        if (auto flushed = file.flush(); !flushed) {
            return std::unexpected(flushed.error());
        }
    }
    // WAL durável: não remove. Checkpoint avança no Database após reopen.
    return result;
}

} // namespace modb::tx
