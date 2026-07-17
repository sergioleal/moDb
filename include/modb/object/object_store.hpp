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
    [[nodiscard]] Result<TypeDefinitionId> register_type(TypeDefinition definition);
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
    [[nodiscard]] Result<ObjectId> create_object(const TypeDefinition& type, FieldValues fields);
    // Recupera um objeto vivo pelo id.
    [[nodiscard]] Result<DecodedObject> get(ObjectId id);
    // Substitui o conteúdo de um objeto, preservando sua identidade.
    [[nodiscard]] Result<void> update(ObjectId id, const TypeDefinition& type, FieldValues fields);
    // Remove um objeto (o id nunca é reutilizado).
    [[nodiscard]] Result<void> remove(ObjectId id);
    // Visita cada objeto de dados vivo, na ordem física.
    [[nodiscard]] Result<void> scan(
        const std::function<Result<void>(const DecodedObject&)>& visitor);

    // Baseline corrente (nullopt antes de qualquer tipo ser registrado).
    [[nodiscard]] const std::optional<Baseline>& current_baseline() const noexcept {
        return current_baseline_;
    }

private:
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
