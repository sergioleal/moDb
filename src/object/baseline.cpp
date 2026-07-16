// Importa a declaração de Baseline.
#include "modb/object/baseline.hpp"

// Disponibiliza std::to_string para as mensagens de erro.
#include <string>
// Disponibiliza um conjunto usado para detectar TypeDefinitionIds repetidos.
#include <unordered_set>

namespace modb::object {

// Cria uma Baseline somente quando a lista de tipos é internamente válida.
Result<Baseline> Baseline::create(std::vector<TypeDefinitionId> types) {
    // Guarda os ids já visitados para detectar duplicidades e ids inválidos.
    std::unordered_set<std::uint64_t> seen;
    for (const auto& type_id : types) {
        // Um TypeDefinitionId zero nunca identifica um tipo real (ADR-001).
        if (type_id == invalid_object_id) {
            return std::unexpected(
                Error{ErrorCode::invalid_object_id, "baseline cannot reference TypeDefinitionId 0"});
        }
        if (!seen.insert(type_id.value).second) {
            return std::unexpected(Error{
                ErrorCode::duplicate_type,
                "baseline references the same TypeDefinitionId twice: " +
                    std::to_string(type_id.value),
            });
        }
    }
    return Baseline{std::move(types)};
}

} // namespace modb::object
