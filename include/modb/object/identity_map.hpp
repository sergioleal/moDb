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
    // Estado completo de versionamento de uma entrada (Fase 6C): usado pela
    // coleta de lixo para reconciliar cada registro físico com a identidade
    // sem sofrer as recusas de `find`/`current_epoch` quando o objeto está
    // removido — o GC precisa enxergar a versão `previous` de um tombstone
    // para decidir se pode reciclá-la.
    struct VersionInfo {
        bool removed{};                             // `current` é um tombstone
        std::uint64_t current_epoch{};              // época da versão `current`
        std::optional<storage::RecordId> current;   // nullopt se removido
        std::optional<storage::RecordId> previous;  // nullopt se não há previous
        std::uint64_t previous_epoch{};             // época da versão `previous`
    };

    // Aloca e inicializa a página raiz do diretório (IDMD).
    [[nodiscard]] static Result<IdentityMap> create(storage::PageFile& file);
    // Reabre um mapa existente a partir da raiz do diretório.
    [[nodiscard]] static Result<IdentityMap> open(storage::PageFile& file,
                                                  storage::PageId directory_root);

    // Página raiz do diretório, para persistir no DatabaseRoot.
    [[nodiscard]] storage::PageId directory_root() const noexcept { return directory_root_; }

    // Registra a localização de um objeto novo; a entrada precisa estar livre.
    // Aloca páginas IDMP/IDMD sob demanda. `epoch` é a época em que este
    // objeto passa a existir (Fase 6B): a época do commit que o tornará
    // durável, nunca observável antes disso (rollback descarta a página junto
    // com o resto da transação).
    [[nodiscard]] Result<void> bind(ObjectId id, storage::RecordId record, std::uint64_t epoch);
    // Devolve a localização "current" de um objeto vivo; ausente ou removido é
    // erro. Não considera `previous` — é a leitura sem Snapshot (Fase 6B).
    [[nodiscard]] Result<storage::RecordId> find(ObjectId id) const;
    // Resolve a localização visível a partir de uma época de snapshot (Fase
    // 6B): `current` se `current_epoch <= snapshot_epoch` (e não removido a
    // essa altura); senão `previous`, se `previous_epoch <= snapshot_epoch`;
    // senão o objeto não existia (ou já não é resolvível) nessa época.
    [[nodiscard]] Result<storage::RecordId> find_at(ObjectId id, std::uint64_t snapshot_epoch) const;
    // Época em que a versão `current` da entrada passou a valer. Usado pelo
    // ObjectStore para decidir se uma nova escrita conflita com um snapshot
    // aberto mais antigo que ela (Fase 6B).
    [[nodiscard]] Result<std::uint64_t> current_epoch(ObjectId id) const;
    // Informa se a entrada já tem uma versão `previous` ocupada — usado pelo
    // ObjectStore na mesma decisão de conflito.
    [[nodiscard]] Result<bool> has_previous(ObjectId id) const;
    // Move a localização `current` para `previous` (sobrescrevendo o que
    // houver lá) e grava a nova localização como `current`, na nova época.
    // Operação puramente mecânica: quem decide se isto é seguro (não há
    // snapshot aberto que ainda precise do `previous` que será sobrescrito) é
    // o chamador (ObjectStore), antes de invocar isto.
    [[nodiscard]] Result<void> rebind(ObjectId id, storage::RecordId record, std::uint64_t epoch);
    // Mesma mecânica de `rebind`, mas `current` passa a representar uma
    // remoção (sem localização física) na nova época. O registro físico
    // antigo não é tocado aqui — permanece acessível via `previous` até a
    // Fase 6C decidir que pode ser reciclado.
    [[nodiscard]] Result<void> erase(ObjectId id, std::uint64_t epoch);
    // Devolve o estado completo de versionamento da entrada (Fase 6C).
    // Diferente de `find`/`current_epoch`/`has_previous`, funciona mesmo com o
    // objeto removido: o GC precisa inspecionar a `previous` de um tombstone.
    [[nodiscard]] Result<VersionInfo> inspect(ObjectId id) const;
    // Descarta a versão `previous` da entrada (Fase 6C), mantendo `current`
    // intacto. Chamado pelo GC depois de reciclar fisicamente o registro
    // anterior, quando nenhum snapshot aberto ainda pode enxergá-lo.
    [[nodiscard]] Result<void> clear_previous(ObjectId id);

private:
    IdentityMap(storage::PageFile& file, storage::PageId directory_root) noexcept
        : file_{&file}, directory_root_{directory_root} {}

    // Resolve a página IDMP que contém a entrada `entry_page` sem alocar nada;
    // nullopt quando o diretório ou a página ainda não existem.
    [[nodiscard]] Result<std::optional<storage::PageId>> resolve_idmp(
        std::uint64_t entry_page) const;
    // Garante (criando se preciso) a página IDMP que contém `entry_page`.
    [[nodiscard]] Result<storage::PageId> ensure_idmp(std::uint64_t entry_page);
    // Regrava um mapa v1 em páginas v2 e devolve a nova raiz de diretório.
    [[nodiscard]] static Result<IdentityMap> migrate_v1(storage::PageFile& file,
                                                         storage::PageId directory_root);

    // Arquivo cuja vida é controlada pelo chamador.
    storage::PageFile* file_;
    // Página raiz do diretório encadeado.
    storage::PageId directory_root_;
};

} // namespace modb::object
