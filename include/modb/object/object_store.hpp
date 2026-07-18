#pragma once

// Importa Result e os códigos de erro.
#include "modb/error.hpp"
// Importa os subsistemas que o ObjectStore orquestra.
#include "modb/object/baseline.hpp"
#include "modb/object/catalog_store.hpp"
#include "modb/object/database_root.hpp"
#include "modb/object/identity_map.hpp"
#include "modb/object/object_codec.hpp"
#include "modb/object/type_definition.hpp"
#include "modb/object/type_registry.hpp"
// Importa PageFile e o TableHeap de dados.
#include "modb/storage/page_file.hpp"
#include "modb/storage/table_heap.hpp"
// Importa o gerador de streaming (Fase 7A).
#include "modb/query/generator.hpp"

// Disponibiliza std::size_t no resultado do GC.
#include <cstddef>
// Disponibiliza std::function para o callback de scan.
#include <functional>
// Disponibiliza std::reference_wrapper nas buscas de tipo.
#include <functional>
// Disponibiliza a baseline corrente opcional.
#include <optional>
// Disponibiliza uma visão leve nas buscas por nome.
#include <string_view>
// Disponibiliza a lista de ids de tipo.
#include <vector>

namespace modb::object {

class Database;

// Amarra o modelo de objetos ao armazenamento persistente: aloca ObjectIds,
// codifica/decodifica objetos, mantém o mapa de identidade, o catálogo e o
// heap de dados. É o primeiro caminho vertical OO — um objeto criado aqui
// sobrevive a fechar e reabrir o arquivo.
//
// O caller possui o PageFile e deve mantê-lo vivo enquanto o ObjectStore
// existir (mesmo contrato de TableHeap).
class ObjectStore {
public:
    // Cria toda a hierarquia num arquivo novo (DBRT, mapa, heaps).
    [[nodiscard]] static Result<ObjectStore> create(storage::PageFile& file);
    // Reabre a hierarquia e reconstrói o catálogo em memória.
    [[nodiscard]] static Result<ObjectStore> open(storage::PageFile& file);

    // Registra um tipo novo, atribuindo seu TypeDefinitionId a partir do
    // contador persistente, gravando-o no catálogo e atualizando a baseline.
    // Busca um tipo registrado por id ou por nome.
    [[nodiscard]] Result<std::reference_wrapper<const TypeDefinition>> find_type(
        TypeDefinitionId id) const;
    [[nodiscard]] Result<std::reference_wrapper<const TypeDefinition>> find_type(
        std::string_view name) const;
    [[nodiscard]] std::vector<std::reference_wrapper<const TypeDefinition>> type_history(
        std::string_view name) const {
        return registry_.history(name);
    }
    // Busca uma baseline histórica por identidade.
    [[nodiscard]] Result<std::reference_wrapper<const Baseline>> find_baseline(
        BaselineId id) const;
    [[nodiscard]] std::span<const Baseline> baselines() const noexcept { return baselines_; }

    // Cria um objeto do tipo dado, validando o payload, persistindo-o e
    // registrando sua identidade. Devolve o ObjectId atribuído.
    // Recupera a versão "current" de um objeto vivo pelo id (sem snapshot).
    [[nodiscard]] Result<DecodedObject> get(ObjectId id);
    // Recupera o objeto tal como era visível numa época de snapshot (Fase
    // 6B): current se já valia nessa época, senão previous, senão ausente.
    [[nodiscard]] Result<DecodedObject> get_at(ObjectId id, std::uint64_t snapshot_epoch);
    // Substitui o conteúdo de um objeto, preservando sua identidade.
    // Remove um objeto (o id nunca é reutilizado).
    // Visita cada objeto de dados vivo, na ordem física (versão "current").
    [[nodiscard]] Result<void> scan(
        const std::function<Result<void>(const DecodedObject&)>& visitor);
    // Visita exatamente os objetos visíveis numa época de snapshot. Como o
    // heap físico pode conter, para um mesmo ObjectId, tanto a versão current
    // quanto uma versão previous ainda preservada, cada registro físico é
    // comparado com o que `find_at` resolveria para aquele id — só o que
    // corresponde à localização física do registro é visitado (evita
    // duplicar ou expor a versão errada).
    [[nodiscard]] Result<void> scan_at(
        std::uint64_t snapshot_epoch,
        const std::function<Result<void>(const DecodedObject&)>& visitor);

    // Fonte de scan PREGUIÇOSA (Fase 7A): um Generator que percorre o heap
    // página a página, cedendo cada objeto visível na época do snapshot (mesma
    // regra de `scan_at`: filtra por identidade para não expor versões previous
    // nem cópias órfãs). Lê uma página de dados só quando o consumidor a
    // alcança — a base do critério TTFR (`limit 1` lê ≤ 2 páginas). `type`
    // nullopt visita todos os tipos.
    [[nodiscard]] query::Generator<Result<DecodedObject>> scan_stream(
        std::uint64_t snapshot_epoch, std::optional<TypeDefinitionId> type);
    // Páginas de dados lidas pelo scan preguiçoso desde o último reset
    // (instrumentação do critério TTFR da Fase 7A).
    [[nodiscard]] std::uint64_t data_pages_read() const noexcept {
        return data_heap_.data_pages_read();
    }
    void reset_data_pages_read() noexcept { data_heap_.reset_data_pages_read(); }

    // Baseline corrente (nullopt antes de qualquer tipo ser registrado).
    [[nodiscard]] const std::optional<Baseline>& current_baseline() const noexcept {
        return current_baseline_;
    }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return root_.epoch(); }
    // Total de registros físicos vivos no heap de dados (Fase 6C): inclui as
    // versões `previous` ainda preservadas e cópias órfãs ainda não coletadas.
    // Diagnóstico read-only, usado por testes e pela CLI para observar o efeito
    // do GC.
    [[nodiscard]] std::uint64_t data_record_count() const noexcept {
        return data_heap_.record_count();
    }
    // Estado de versionamento de um objeto (Fase 6): época/localização current
    // e, se houver, a versão `previous` retida. Diagnóstico read-only para a
    // CLI (`mvcc versions`).
    [[nodiscard]] Result<IdentityMap::VersionInfo> version_info(ObjectId id) const {
        return identity_.inspect(id);
    }

    // Próximo ObjectId a ser alocado (contador persistido). Exposto para o
    // rollback de transação (Fase 5) preservar a garantia de não-reuso mesmo
    // depois de reconstruir o ObjectStore a partir do disco.
    [[nodiscard]] std::uint64_t next_object_id_watermark() const noexcept {
        return root_.next_object_id();
    }
    // Garante que o contador nunca retroceda: se `at_least` for maior que o
    // valor persistido atual, avança e o grava. Seguro fora de uma transação
    // — não vaza outro campo do DBRT ainda em voo; o Database faz o flush
    // subsequente quando este caminho é usado no rollback.
    [[nodiscard]] Result<void> ensure_next_object_id_at_least(std::uint64_t at_least) {
        if (at_least <= root_.next_object_id()) {
            return {};
        }
        return root_.set_next_object_id(at_least);
    }

private:
    friend class Database;
    [[nodiscard]] Result<TypeDefinitionId> register_type(TypeDefinition definition);
    [[nodiscard]] Result<ObjectId> create_object(const TypeDefinition& type, FieldValues fields);
    // `oldest_open_snapshot_epoch` é a época do snapshot aberto mais antigo no
    // momento da escrita (nullopt se nenhum estiver aberto) — decide se esta
    // escrita conflita com uma versão `previous` ainda visível (Fase 6B).
    [[nodiscard]] Result<void> update(ObjectId id, const TypeDefinition& type, FieldValues fields,
                                      std::optional<std::uint64_t> oldest_open_snapshot_epoch);
    [[nodiscard]] Result<void> remove(ObjectId id,
                                      std::optional<std::uint64_t> oldest_open_snapshot_epoch);
    // Recicla o espaço físico das versões antigas que nenhum snapshot aberto
    // ainda pode enxergar (Fase 6C). Reconcilia cada registro do heap com a
    // identidade: a versão `current` viva nunca é tocada; uma versão `previous`
    // é liberada (e a entrada compactada com `clear_previous`) quando
    // `oldest_open_snapshot_epoch` é nulo ou >= à época `current` da entrada;
    // qualquer registro que não seja nem `current` nem a `previous` referenciada
    // é uma cópia órfã (ex.: previous sobrescrito por uma segunda alteração) e
    // é sempre liberado. Exige uma transação ativa (as liberações passam pelo
    // WAL). Devolve quantos registros físicos foram recuperados.
    [[nodiscard]] Result<std::size_t> collect_garbage(
        std::optional<std::uint64_t> oldest_open_snapshot_epoch);
    [[nodiscard]] Result<void> advance_epoch() { return root_.advance_epoch(); }
    // Verifica se uma escrita em `id` conflitaria com um snapshot ainda aberto
    // que dependa da versão `previous` (Fase 6B): só é possível quando já há
    // uma `previous` ocupada E existe um snapshot mais antigo que a época
    // `current` da entrada — nesse caso não sobra posição para a nova versão
    // que este `previous` teria de virar.
    [[nodiscard]] Result<void> check_snapshot_conflict(
        ObjectId id, std::optional<std::uint64_t> oldest_open_snapshot_epoch) const;
    ObjectStore(storage::PageFile& file, DatabaseRoot root, IdentityMap identity,
                storage::TableHeap data_heap, CatalogStore catalog, TypeRegistry registry,
                std::vector<TypeDefinitionId> type_ids, std::vector<Baseline> baselines,
                std::optional<Baseline> baseline) noexcept
        : file_{&file},
          root_{std::move(root)},
          identity_{std::move(identity)},
          data_heap_{std::move(data_heap)},
          catalog_{std::move(catalog)},
          registry_{std::move(registry)},
          type_ids_{std::move(type_ids)},
          baselines_{std::move(baselines)},
          current_baseline_{std::move(baseline)} {}

    // Aloca o próximo ObjectId, persistindo o contador antes de devolvê-lo.
    [[nodiscard]] Result<ObjectId> allocate_object_id();

    // Arquivo cuja vida é controlada pelo chamador.
    storage::PageFile* file_;
    DatabaseRoot root_;
    IdentityMap identity_;
    storage::TableHeap data_heap_;
    CatalogStore catalog_;
    TypeRegistry registry_;
    // Ids das versões ativas, usados ao construir a próxima baseline.
    std::vector<TypeDefinitionId> type_ids_;
    // Baselines históricas imutáveis, inclusive a corrente.
    std::vector<Baseline> baselines_;
    std::optional<Baseline> current_baseline_;
};

} // namespace modb::object
