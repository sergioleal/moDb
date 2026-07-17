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
// Disponibiliza std::error_code na remoção do WAL.
#include <system_error>
// Disponibiliza o conjunto de transações commitadas.
#include <unordered_set>

namespace modb::tx {

Result<void> recover(storage::PageFile& file, const std::filesystem::path& wal_path) {
    // Sem WAL, não há nada a recuperar (caso normal de banco fechado limpo).
    std::error_code exists_error;
    if (!std::filesystem::exists(wal_path, exists_error)) {
        return {};
    }

    auto records = Wal::read_all(wal_path);
    if (!records) {
        // Um WAL ilegível pode conter um commit durável. Não o apaga: interrompe
        // a abertura para preservar a evidência e permitir diagnóstico/repair.
        return std::unexpected(Error{ErrorCode::wal_corrupt,
                                     "cannot recover unreadable WAL: " +
                                         records.error().message});
    }

    // Identifica quais transações alcançaram o registro de commit.
    std::unordered_set<std::uint64_t> committed;
    for (const auto& record : *records) {
        if (record.type == WalRecordType::commit) {
            committed.insert(record.tx_id);
        }
    }

    // Reaplica as imagens de página das transações commitadas, na ordem do log
    // (LSN crescente). A reaplicação é idempotente: escrever a mesma página
    // duas vezes leva ao mesmo estado.
    bool applied_any = false;
    for (const auto& record : *records) {
        if (record.type != WalRecordType::page_image || !committed.contains(record.tx_id)) {
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
        applied_any = true;
    }
    if (applied_any) {
        if (auto flushed = file.flush(); !flushed) {
            return std::unexpected(flushed.error());
        }
    }

    // Checkpoint: as páginas commitadas estão duráveis no arquivo de dados; o
    // WAL pode ser removido. Uma nova queda antes desta remoção apenas reaplica
    // (idempotente) na próxima abertura.
    std::error_code remove_error;
    std::filesystem::remove(wal_path, remove_error);
    if (remove_error) {
        // As páginas já foram durabilizadas; manter o WAL é seguro para redo
        // idempotente, mas a abertura informa o checkpoint incompleto.
        return std::unexpected(
            Error{ErrorCode::io_error, "could not remove recovered WAL: " + remove_error.message()});
    }
    return {};
}

} // namespace modb::tx
