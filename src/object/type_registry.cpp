// Importa a declaração de TypeRegistry.
#include "modb/object/type_registry.hpp"

// Disponibiliza std::to_string para as mensagens de erro.
#include <string>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {

// Registra um tipo, validando unicidade de nome e atribuindo seu id.
Result<TypeDefinitionId> TypeRegistry::register_type(TypeDefinition definition) {
    // O nome já precisa ter sido validado por TypeDefinition::create; aqui só
    // resta checar a unicidade entre os tipos já conhecidos deste registro.
    if (latest_id_by_name_.contains(definition.name())) {
        return std::unexpected(
            Error{ErrorCode::duplicate_type, "type already registered: " + definition.name()});
    }

    // Copia o nome antes de mover a definição para dentro do mapa por id.
    const std::string name = definition.name();
    const TypeDefinitionId id{next_id_};
    ++next_id_;

    // Estampa o id sobre a definição já validada, sem revalidar nada.
    TypeDefinition stamped{id, std::move(definition)};
    types_by_id_.emplace(id.value, std::move(stamped));
    latest_id_by_name_.emplace(name, id.value);
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
    if (latest_id_by_name_.contains(definition.name())) {
        return std::unexpected(
            Error{ErrorCode::duplicate_type, "type already registered: " + definition.name()});
    }
    const std::string name = definition.name();
    TypeDefinition stamped{id, std::move(definition)};
    types_by_id_.emplace(id.value, std::move(stamped));
    latest_id_by_name_.emplace(name, id.value);
    // Mantém o contador interno à frente de qualquer id já visto, para o caso de
    // o mesmo registro também ser usado com register_type (auto id).
    if (id.value >= next_id_) {
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

} // namespace modb::object
