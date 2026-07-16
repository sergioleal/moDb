#pragma once

// Importa Result e os códigos de erro.
#include "modb/error.hpp"
// Importa Baseline, TypeDefinition e os ids.
#include "modb/object/baseline.hpp"
#include "modb/object/ids.hpp"
#include "modb/object/type_definition.hpp"
// Importa PageFile/PageId e o TableHeap que guarda os objetos de catálogo.
#include "modb/storage/page_file.hpp"
#include "modb/storage/table_heap.hpp"

// Disponibiliza os vetores do conteúdo lido.
#include <vector>

namespace modb::object {

// Um TypeDefinition lido do catálogo, com o id persistido separado. A definição
// vem sem id estampado (id() == 0); quem carrega decide como registrá-la.
struct DecodedType {
    TypeDefinitionId id;
    TypeDefinition definition;
};

// Uma Baseline lida do catálogo, com seu id persistido.
struct DecodedBaseline {
    BaselineId id;
    Baseline baseline;
};

// Todo o conteúdo do heap de catálogo após uma varredura.
struct CatalogContents {
    std::vector<DecodedType> types;
    std::vector<DecodedBaseline> baselines;
};

// Persiste e recupera os objetos do catálogo (TypeDefinition e Baseline) num
// TableHeap dedicado, usando o codec genérico com os meta-tipos reservados
// (ADR-002). Os objetos de catálogo são encontrados por varredura, não pelo
// mapa de identidade.
class CatalogStore {
public:
    // Cria o TableHeap do catálogo.
    [[nodiscard]] static Result<CatalogStore> create(storage::PageFile& file);
    // Reabre o TableHeap do catálogo a partir da sua raiz.
    [[nodiscard]] static Result<CatalogStore> open(storage::PageFile& file,
                                                   storage::PageId heap_root);

    // Raiz do heap de catálogo, para persistir no DatabaseRoot.
    [[nodiscard]] storage::PageId heap_root() const noexcept { return heap_.root_page(); }

    // Grava um TypeDefinition já estampado com seu id.
    [[nodiscard]] Result<void> save_type(const TypeDefinition& definition);
    // Grava uma Baseline já estampada com seu id.
    [[nodiscard]] Result<void> save_baseline(const Baseline& baseline);
    // Varre o heap e decodifica todos os objetos de catálogo.
    [[nodiscard]] Result<CatalogContents> load_all();

private:
    explicit CatalogStore(storage::TableHeap heap) noexcept : heap_{std::move(heap)} {}

    // Heap dedicado aos objetos de catálogo.
    storage::TableHeap heap_;
};

} // namespace modb::object
