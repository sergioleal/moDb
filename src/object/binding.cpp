// Importa a interface de Binding.
#include "modb/object/binding.hpp"

// Importa o codec de valor único, reusado no sub-formato embedded.
#include "modb/object/object_codec.hpp"
// Importa o teto de campos (ADR-007).
#include "modb/limits.hpp"
// Importa BinaryWriter/BinaryReader do sub-formato embedded.
#include "modb/storage/binary.hpp"

// Disponibiliza std::min ao limitar a reserva do sub-formato.
#include <algorithm>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {

Result<TypeDefinition> Binding::to_type_definition() const {
    std::vector<AttributeDefinition> attributes;
    attributes.reserve(fields_.size());
    for (const auto& binder : fields_) {
        // Um campo de binding mapeia um membro sempre presente e pode declarar
        // o default usado ao projetar objetos de versões anteriores. As flags
        // de relacionamento (owned/embedded) fazem parte da estrutura do tipo.
        attributes.push_back(AttributeDefinition{
            .id = binder.id,
            .name = binder.name,
            .type = binder.type,
            .nullable = false,
            .default_value = binder.default_value,
            .is_embedded = binder.is_embedded,
            .is_owned = binder.is_owned});
    }
    // Reaproveita toda a validação de TypeDefinition::create (limites, ids, etc.).
    return TypeDefinition::create(type_name_, std::move(attributes));
}

Result<FieldValues> Binding::to_field_values(const void* object) const {
    FieldValues values;
    if (object == nullptr) {
        return values;
    }
    values.reserve(fields_.size());
    for (const auto& binder : fields_) {
        auto value = binder.load(object);
        if (!value) {
            return std::unexpected(value.error());
        }
        values.emplace_back(binder.id, std::move(*value));
    }
    return values;
}

Result<std::vector<std::byte>> encode_embedded(const FieldValues& fields) {
    if (fields.size() > modb::max_columns_per_table) {
        return std::unexpected(
            Error{ErrorCode::too_many_columns, "embedded object has too many fields"});
    }
    storage::BinaryWriter writer;
    writer.write_u16(static_cast<std::uint16_t>(fields.size()));
    for (const auto& [id, value] : fields) {
        writer.write_u16(id.value);
        if (auto encoded = encode_value(writer, value); !encoded) {
            return std::unexpected(encoded.error());
        }
    }
    return std::move(writer).take();
}

Result<FieldValues> decode_embedded(std::span<const std::byte> payload) {
    storage::BinaryReader reader{payload};
    auto count = reader.read_u16();
    if (!count) {
        return std::unexpected(count.error());
    }
    FieldValues fields;
    fields.reserve(std::min<std::size_t>(*count, modb::max_columns_per_table));
    for (std::uint16_t index = 0; index < *count; ++index) {
        auto field_id = reader.read_u16();
        if (!field_id) {
            return std::unexpected(field_id.error());
        }
        auto value = decode_value(reader);
        if (!value) {
            return std::unexpected(value.error());
        }
        fields.emplace_back(FieldId{*field_id}, std::move(*value));
    }
    if (!reader.at_end()) {
        return std::unexpected(
            Error{ErrorCode::trailing_data, "embedded object has trailing bytes"});
    }
    return fields;
}

Result<void> Binding::materialize(const FieldValues& fields, void* destination) const {
    if (destination == nullptr) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "binding destination cannot be null"});
    }
    for (const auto& binder : fields_) {
        // Localiza o valor persistido correspondente a este campo.
        const AttributeValue* value = nullptr;
        for (const auto& [field_id, candidate] : fields) {
            if (field_id == binder.id) {
                value = &candidate;
                break;
            }
        }
        if (value == nullptr) {
            return std::unexpected(Error{ErrorCode::field_not_found,
                                         "stored object is missing bound field: " + binder.name});
        }
        if (auto stored = binder.store(destination, *value); !stored) {
            return std::unexpected(stored.error());
        }
    }
    return {};
}

} // namespace modb::object
