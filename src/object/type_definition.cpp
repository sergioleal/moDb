// Importa TypeDefinition, AttributeDefinition e validate_object.
#include "modb/object/type_definition.hpp"

// Importa os limites compartilhados de identificador e contagem (ADR-007).
#include "modb/limits.hpp"

// Disponibiliza a montagem de mensagens com números.
#include <string>
// Disponibiliza um conjunto usado para detectar FieldIds e nomes repetidos.
#include <unordered_set>

namespace modb::object {
namespace {

// Valida apenas as regras de um identificador (nome de tipo ou de atributo).
Result<void> validate_identifier(std::string_view name) {
    // Reaproveita o limite compartilhado com o restante do produto (ADR-007).
    if (name.empty() || name.size() > modb::max_identifier_bytes) {
        return std::unexpected(Error{
            ErrorCode::invalid_identifier,
            "identifier must contain between 1 and " +
                std::to_string(modb::max_identifier_bytes) + " bytes",
        });
    }
    return {};
}

// Confere se um default declarado é compatível com o restante do atributo.
Result<void> validate_default(const AttributeDefinition& attribute) {
    if (!attribute.default_value) {
        return {};
    }
    // Coleções e objetos embutidos não têm defaults nesta fase: coleções têm
    // armazenamento próprio (Fase 4) e embutidos não têm tag de storage ainda.
    if (attribute.is_collection || attribute.is_embedded) {
        return std::unexpected(Error{
            ErrorCode::invalid_argument,
            "collection and embedded attributes cannot have a default value: " +
                attribute.name,
        });
    }
    const auto default_type = attribute.default_value->type();
    if (default_type == AttributeType::null) {
        // Um default null só faz sentido se o atributo aceitar null.
        if (!attribute.nullable) {
            return std::unexpected(Error{
                ErrorCode::null_constraint_violation,
                "default value is null but attribute is not nullable: " + attribute.name,
            });
        }
        return {};
    }
    if (default_type != attribute.type) {
        return std::unexpected(Error{
            ErrorCode::type_mismatch,
            "default value type does not match attribute type: " + attribute.name,
        });
    }
    return {};
}

} // namespace

// Cria um TypeDefinition somente quando nome e atributos são válidos.
Result<TypeDefinition> TypeDefinition::create(std::string name,
                                              std::vector<AttributeDefinition> attributes) {
    if (auto result = validate_identifier(name); !result) {
        return std::unexpected(result.error());
    }
    // Reaproveita o mesmo teto de colunas do modelo relacional (ADR-007).
    if (attributes.size() > modb::max_columns_per_table) {
        return std::unexpected(Error{
            ErrorCode::too_many_columns,
            "type cannot contain more than " + std::to_string(modb::max_columns_per_table) +
                " attributes",
        });
    }

    // Guarda FieldIds e nomes já visitados para detectar duplicidades.
    std::unordered_set<std::uint16_t> field_ids;
    std::unordered_set<std::string> names;
    for (const auto& attribute : attributes) {
        // FieldId zero é reservado como marcador de "nenhum campo".
        if (attribute.id.value == 0) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "FieldId must not be zero: " + attribute.name});
        }
        if (!field_ids.insert(attribute.id.value).second) {
            return std::unexpected(Error{
                ErrorCode::duplicate_field,
                "duplicate FieldId " + std::to_string(attribute.id.value) + " in type " + name,
            });
        }
        if (auto result = validate_identifier(attribute.name); !result) {
            return std::unexpected(result.error());
        }
        if (!names.insert(attribute.name).second) {
            return std::unexpected(
                Error{ErrorCode::duplicate_column, "duplicate attribute name: " + attribute.name});
        }
        if (auto result = validate_default(attribute); !result) {
            return std::unexpected(result.error());
        }
    }

    // Move o vetor validado para dentro da nova definição (id ainda {0}).
    return TypeDefinition{std::move(name), std::move(attributes)};
}

// Procura um atributo comparando o FieldId.
const AttributeDefinition* TypeDefinition::find(FieldId id) const noexcept {
    for (const auto& attribute : attributes_) {
        if (attribute.id == id) {
            return &attribute;
        }
    }
    return nullptr;
}

// Procura um atributo comparando o nome.
const AttributeDefinition* TypeDefinition::find(std::string_view name) const noexcept {
    for (const auto& attribute : attributes_) {
        if (attribute.name == name) {
            return &attribute;
        }
    }
    return nullptr;
}

// Confere se os campos fornecidos respeitam o tipo antes de qualquer escrita.
Result<void> validate_object(const TypeDefinition& type, const FieldValues& fields) {
    // Registra quais atributos já foram vistos para checar duplicatas e, ao
    // final, quais ficaram sem valor.
    std::unordered_set<std::uint16_t> seen;

    for (const auto& [field_id, value] : fields) {
        if (!seen.insert(field_id.value).second) {
            return std::unexpected(Error{
                ErrorCode::duplicate_field,
                "duplicate FieldId " + std::to_string(field_id.value) + " in object payload",
            });
        }
        const auto* attribute = type.find(field_id);
        if (attribute == nullptr) {
            return std::unexpected(Error{
                ErrorCode::field_not_found,
                "type " + type.name() + " has no attribute with FieldId " +
                    std::to_string(field_id.value),
            });
        }
        if (value.is_null()) {
            if (!attribute->nullable) {
                return std::unexpected(Error{
                    ErrorCode::null_constraint_violation,
                    "attribute does not accept null: " + attribute->name,
                });
            }
            continue;
        }
        if (value.type() != attribute->type) {
            return std::unexpected(Error{
                ErrorCode::type_mismatch,
                "invalid value type for attribute: " + attribute->name,
            });
        }
    }

    // Um atributo ausente do payload só é aceitável se puder ser completado
    // sem perda de informação: por default, ou porque aceita null implícito.
    for (const auto& attribute : type.attributes()) {
        if (seen.contains(attribute.id.value)) {
            continue;
        }
        if (attribute.default_value.has_value()) {
            continue;
        }
        if (!attribute.nullable) {
            return std::unexpected(Error{
                ErrorCode::null_constraint_violation,
                "attribute is missing, not nullable, and has no default: " + attribute.name,
            });
        }
    }
    return {};
}

} // namespace modb::object
