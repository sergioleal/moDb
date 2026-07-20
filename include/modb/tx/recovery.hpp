#pragma once

// Importa Result e os códigos de erro da recuperação.
#include "modb/error.hpp"

// Disponibiliza caminhos do arquivo WAL.
#include <filesystem>

namespace modb::storage {
class PageFile;
} // namespace modb::storage

namespace modb::tx {

// Resultado da recuperação redo-only (Fase 14B: WAL durável).
struct RecoverResult {
    // Maior commit_lsn reaplicado (0 se nada foi aplicado).
    std::uint64_t max_commit_lsn{0};
    bool applied_any{false};
};

// Reaplica o WAL na abertura do banco (redo-only). Se o arquivo não existir,
// não há nada a fazer. Transações com `commit` e lsn > `after_lsn` têm imagens
// reaplicadas de forma idempotente; sem `commit` são descartadas.
// O WAL **não** é removido (checkpoint é posição no DBRT).
[[nodiscard]] Result<RecoverResult> recover(storage::PageFile& file,
                                            const std::filesystem::path& wal_path,
                                            std::uint64_t after_lsn = 0);

} // namespace modb::tx
