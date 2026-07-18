#pragma once

// Linha projetada da Fase 7C: só os campos pedidos (e/ou valores computados),
// sem materializar o objeto C++ completo no consumidor. Streaming — uma linha
// por vez. Identidade, atributos e valores computados têm a mesma representação
// uniforme em `fields`; campos sintéticos usam FieldId 0.

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
