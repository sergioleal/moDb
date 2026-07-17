// Importa a declaração de TypeRegistry.
#include "modb/object/type_registry.hpp"

// Disponibiliza o limite do contador de ids.
#include <limits>
// Disponibiliza ordenação do histórico por identidade.
#include <algorithm>
// Disponibiliza std::to_string para as mensagens de erro.
#include <string>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {

// Registra uma nova versão de tipo e atribui seu id.
Result<TypeDefinitionId> TypeRegistry::register_type(TypeDefinition definition) {
    if (next_id_ == 0 || next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(
            Error{ErrorCode::value_too_large, "TypeDefinitionId space exhausted"});
    }
    // Copia o nome antes de mover a definição para dentro do mapa por id.
    const std::string name = definition.name();
    const TypeDefinitionId id{next_id_};
    ++next_id_;

    // Estampa o id sobre a definição já validada, sem revalidar nada.
    TypeDefinition stamped{id, std::move(definition)};
    types_by_id_.emplace(id.value, std::move(stamped));
    latest_id_by_name_.insert_or_assign(name, id.value);
    return id;
}

// Registra um tipo com um id decidido externamente (contador do DBRT).
Result<void> TypeRegistry::register_with_id(TypeDefinitionId id, TypeDefinition definition) {
    if (id == invalid_object_id) {
        return std::unexpected(
            Error{ErrorCode::invalid_object_id, "cannot register a type with id 0"});
    }
    if (types_by_id_.contains(id.value)) {
        return std::unexpected(Error{ErrorCode::duplicate_type,
                                     "type id already registered: " + std::to_string(id.value)});
    }
    const std::string name = definition.name();
    TypeDefinition stamped{id, std::move(definition)};
    types_by_id_.emplace(id.value, std::move(stamped));
    // A maior identidade é a versão mais recente. Isso também torna a
    // reconstrução independente da ordem física dos registros do catálogo.
    const auto latest = latest_id_by_name_.find(name);
    if (latest == latest_id_by_name_.end() || id.value > latest->second) {
        latest_id_by_name_.insert_or_assign(name, id.value);
    }
    // Mantém o contador interno à frente de qualquer id já visto, para o caso de
    // o mesmo registro também ser usado com register_type (auto id).
    if (id.value == std::numeric_limits<std::uint64_t>::max()) {
        next_id_ = 0;
    } else if (id.value >= next_id_) {
        next_id_ = id.value + 1;
    }
    return {};
}

// Procura um tipo pelo identificador atribuído no registro.
Result<std::reference_wrapper<const TypeDefinition>> TypeRegistry::find(
    TypeDefinitionId id) const {
    const auto it = types_by_id_.find(id.value);
    if (it == types_by_id_.end()) {
        return std::unexpected(
            Error{ErrorCode::type_not_found, "type not found: id " + std::to_string(id.value)});
    }
    return std::cref(it->second);
}

// Procura a versão mais recente de um tipo pelo nome.
Result<std::reference_wrapper<const TypeDefinition>> TypeRegistry::find(
    std::string_view name) const {
    const auto it = latest_id_by_name_.find(std::string{name});
    if (it == latest_id_by_name_.end()) {
        return std::unexpected(
            Error{ErrorCode::type_not_found, "type not found: " + std::string{name}});
    }
    return find(TypeDefinitionId{it->second});
}

std::vector<std::reference_wrapper<const TypeDefinition>> TypeRegistry::history(
    std::string_view name) const {
    std::vector<std::reference_wrapper<const TypeDefinition>> versions;
    for (const auto& [id, type] : types_by_id_) {
        (void)id;
        if (type.name() == name) {
            versions.push_back(std::cref(type));
        }
    }
    std::ranges::sort(versions, {}, [](const auto& version) {
        return version.get().id().value;
    });
    return versions;
}

Result<void> TypeRegistry::activate(std::span<const TypeDefinitionId> type_ids) {
    std::unordered_map<std::string, std::uint64_t> active;
    active.reserve(type_ids.size());
    for (const auto id : type_ids) {
        auto type = find(id);
        if (!type) {
            return std::unexpected(type.error());
        }
        if (!active.emplace(type->get().name(), id.value).second) {
            return std::unexpected(
                Error{ErrorCode::duplicate_type, "baseline contains two versions of type: " +
                                                     type->get().name()});
        }
    }
    latest_id_by_name_ = std::move(active);
    return {};
}

} // namespace modb::object
