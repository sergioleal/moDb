#pragma once

// Importa Result e os códigos de erro.
#include "modb/error.hpp"
// Importa ObjectId.
#include "modb/object/ids.hpp"
// Importa PageFile, PageId e RecordId.
#include "modb/storage/page_file.hpp"
#include "modb/storage/slotted_page.hpp"

// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza a página IDMP opcional durante a resolução.
#include <optional>

namespace modb::object {

// Traduz ObjectId em localização física (RecordId), de forma persistente
// (ADR-005). Como os ObjectId são densos e nunca reutilizados, o mapa é
// endereçado diretamente pelo id, sem árvore de busca: o id é o índice global.
//
// Duas camadas de páginas: um diretório encadeado (IDMD) cujos slots apontam
// para páginas de entradas (IDMP). Cada entrada guarda page/slot/generation e
// flags (alocada, removida). Um find custa no máximo duas leituras de página.
class IdentityMap {
public:
    // Aloca e inicializa a página raiz do diretório (IDMD).
    [[nodiscard]] static Result<IdentityMap> create(storage::PageFile& file);
    // Reabre um mapa existente a partir da raiz do diretório.
    [[nodiscard]] static Result<IdentityMap> open(storage::PageFile& file,
                                                  storage::PageId directory_root);

    // Página raiz do diretório, para persistir no DatabaseRoot.
    [[nodiscard]] storage::PageId directory_root() const noexcept { return directory_root_; }

    // Registra a localização de um objeto novo; a entrada precisa estar livre.
    // Aloca páginas IDMP/IDMD sob demanda.
    [[nodiscard]] Result<void> bind(ObjectId id, storage::RecordId record);
    // Devolve a localização de um objeto vivo; ausente ou removido é erro.
    [[nodiscard]] Result<storage::RecordId> find(ObjectId id) const;
    // Atualiza a localização de um objeto já registrado (mudou de página).
    [[nodiscard]] Result<void> rebind(ObjectId id, storage::RecordId record);
    // Marca o objeto como removido (tombstone); o id nunca é reutilizado.
    [[nodiscard]] Result<void> erase(ObjectId id);

private:
    IdentityMap(storage::PageFile& file, storage::PageId directory_root) noexcept
        : file_{&file}, directory_root_{directory_root} {}

    // Resolve a página IDMP que contém a entrada `entry_page` sem alocar nada;
    // nullopt quando o diretório ou a página ainda não existem.
    [[nodiscard]] Result<std::optional<storage::PageId>> resolve_idmp(
        std::uint64_t entry_page) const;
    // Garante (criando se preciso) a página IDMP que contém `entry_page`.
    [[nodiscard]] Result<storage::PageId> ensure_idmp(std::uint64_t entry_page);

    // Arquivo cuja vida é controlada pelo chamador.
    storage::PageFile* file_;
    // Página raiz do diretório encadeado.
    storage::PageId directory_root_;
};

} // namespace modb::object
