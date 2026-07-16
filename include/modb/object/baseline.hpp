#pragma once

// Importa Result e os códigos de erro de validação.
#include "modb/error.hpp"
// Importa BaselineId e TypeDefinitionId.
#include "modb/object/ids.hpp"

// Disponibiliza uma visão sem cópia dos tipos.
#include <span>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza o contêiner que possui os TypeDefinitionIds.
#include <vector>

namespace modb::object {

// Declarada aqui para que Baseline possa conceder acesso ao construtor que
// atribui o id (mesma razão de TypeDefinition, ver type_definition.hpp).
class TypeRegistry;

// Representa um snapshot estrutural completo e imutável do catálogo: o
// conjunto de tipos válidos num determinado momento (arquitetura.md §16).
//
// Uma Baseline vazia é válida — representa o catálogo antes de qualquer tipo
// de usuário ser definido.
class Baseline {
public:
    // Cria uma Baseline validada e ainda não registrada (id() == {0}).
    [[nodiscard]] static Result<Baseline> create(std::vector<TypeDefinitionId> types);

    // Retorna o identificador da baseline, ou {0} se ainda não registrada.
    [[nodiscard]] BaselineId id() const noexcept { return id_; }
    // Expõe os tipos somente para leitura e sem cópia.
    [[nodiscard]] std::span<const TypeDefinitionId> types() const noexcept { return types_; }

    // Permite comparar baselines em testes.
    friend bool operator==(const Baseline&, const Baseline&) = default;

private:
    // Mesma razão de TypeDefinition: só quem atribui identidade estampa o id.
    friend class TypeRegistry;

    // Constrói a versão ainda não registrada (id {0}); usado por create().
    explicit Baseline(std::vector<TypeDefinitionId> types) : types_{std::move(types)} {}
    // Estampa um id sobre uma baseline já validada, sem revalidar nada.
    Baseline(BaselineId id, Baseline unassigned) noexcept
        : id_{id}, types_{std::move(unassigned.types_)} {}

    // {0} até a baseline ser registrada.
    BaselineId id_{};
    // TypeDefinitionIds que compõem este snapshot estrutural.
    std::vector<TypeDefinitionId> types_;
};

} // namespace modb::object
