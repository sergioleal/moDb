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
    // Sobre os registros lidos: maior LSN e maior tx_id (0 se nenhum), e o
    // offset do fim do último registro de commit -- tudo depois dele é de
    // transação que não commitou ou é o rabo rasgado de uma queda.
    // `last_commit_end` é 0 quando o WAL não existe.
    std::uint64_t max_lsn{0};
    std::uint64_t max_tx_id{0};
    std::uint64_t last_commit_end{0};
    // true quando a leitura começou em `start_offset` (sem reler o WAL inteiro).
    bool read_from_offset{false};
};

// Reaplica o WAL na abertura do banco (redo-only). Se o arquivo não existir,
// não há nada a fazer. Transações com `commit` e lsn > `after_lsn` têm imagens
// reaplicadas de forma idempotente; sem `commit` são descartadas.
// O WAL **não** é removido (checkpoint é posição no DBRT).
//
// `start_offset`: offset do WAL no último checkpoint (DBRT
// `checkpoint_wal_offset`, T26). Se for válido -- dentro do arquivo e com o
// primeiro registro ali legível e posterior a `after_lsn` -- só o trecho dali
// em diante é lido; senão, o WAL inteiro, como antes.
[[nodiscard]] Result<RecoverResult> recover(storage::PageFile& file,
                                            const std::filesystem::path& wal_path,
                                            std::uint64_t after_lsn = 0,
                                            std::uint64_t start_offset = 0);

} // namespace modb::tx
