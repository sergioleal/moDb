#pragma once

// Importa Result e os códigos de erro da recuperação.
#include "modb/error.hpp"

// Disponibiliza caminhos do arquivo WAL.
#include <filesystem>

namespace modb::storage {
class PageFile;
} // namespace modb::storage

namespace modb::tx {

// Reaplica o WAL na abertura do banco (redo-only). Se o arquivo não existir,
// não há nada a fazer. Transações com `commit` têm suas imagens de página
// reaplicadas de forma idempotente; transações sem `commit` são descartadas.
// Ao final, o arquivo de dados é sincronizado e o WAL é removido.
//
// `wal_path` é o caminho do log; `file` é o PageFile de dados já aberto.
[[nodiscard]] Result<void> recover(storage::PageFile& file,
                                   const std::filesystem::path& wal_path);

} // namespace modb::tx
