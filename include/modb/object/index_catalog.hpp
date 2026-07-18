#pragma once

// Diretório persistente de índices (Fase 7B): uma página `IXDR` que lista, para
// cada índice, o tipo lógico e o campo indexados e a página raiz da B+ tree. A
// raiz de um índice muda quando a árvore cresce em altura; por isso o catálogo
// expõe `set_root`, chamado após cada manutenção que possa ter feito split de
// raiz. O ponteiro para esta página vive no DBRT (`index_dir`).

// Importa Result e códigos de erro.
#include "modb/error.hpp"
// Importa PageFile e PageId.
#include "modb/storage/page_file.hpp"

// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza o nome do tipo indexado.
#include <string>
#include <string_view>
// Disponibiliza a lista de índices.
#include <vector>

namespace modb::object {

// Descreve um índice: tipo lógico + campo + raiz da B+ tree.
struct IndexInfo {
    std::string type_name;
    std::uint16_t field_id{};
    storage::PageId root{};
};

class IndexCatalog {
public:
    // Cria um diretório vazio (uma página nova) e devolve o handle.
    [[nodiscard]] static Result<IndexCatalog> create(storage::PageFile& file);
    // Reabre o diretório a partir da página persistida.
    [[nodiscard]] static Result<IndexCatalog> open(storage::PageFile& file,
                                                   storage::PageId directory);

    // Página do diretório (para persistir no DBRT).
    [[nodiscard]] storage::PageId directory() const noexcept { return directory_; }
    // Índices registrados (espelho em memória).
    [[nodiscard]] const std::vector<IndexInfo>& indexes() const noexcept { return indexes_; }

    // Índice do vetor para (tipo, campo), ou -1 se não existe.
    [[nodiscard]] int find(std::string_view type_name, std::uint16_t field_id) const noexcept;
    // Acrescenta um índice e persiste. Rejeita duplicata (mesmo tipo+campo).
    [[nodiscard]] Result<void> add(IndexInfo info);
    // Atualiza a raiz do índice de posição `slot` e persiste.
    [[nodiscard]] Result<void> set_root(std::size_t slot, storage::PageId root);

private:
    IndexCatalog(storage::PageFile& file, storage::PageId directory,
                 std::vector<IndexInfo> indexes) noexcept
        : file_{&file}, directory_{directory}, indexes_{std::move(indexes)} {}

    [[nodiscard]] Result<void> persist();

    storage::PageFile* file_;
    storage::PageId directory_;
    std::vector<IndexInfo> indexes_;
};

} // namespace modb::object
