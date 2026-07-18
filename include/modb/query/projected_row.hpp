#pragma once

// Linha projetada da Fase 7C: só os campos pedidos (e/ou valores computados),
// sem materializar o objeto C++ completo no consumidor. Streaming — uma linha
// por vez. O ObjectId é metadado de identidade (não é atributo do schema) e
// sempre acompanha a linha, para o consumidor poder correlacionar/atualizar.

#include "modb/object/attribute_value.hpp"
#include "modb/object/ids.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modb::query {

struct ProjectedField {
    object::FieldId id{};
    std::string name;
    object::AttributeValue value;
};

struct ProjectedRow {
    object::ObjectId object_id{};
    std::vector<ProjectedField> fields;

    [[nodiscard]] std::optional<object::AttributeValue> get(object::FieldId id) const {
        for (const auto& field : fields) {
            if (field.id.value == id.value) {
                return field.value;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<object::AttributeValue> get(std::string_view name) const {
        for (const auto& field : fields) {
            if (field.name == name) {
                return field.value;
            }
        }
        return std::nullopt;
    }
};

} // namespace modb::query
