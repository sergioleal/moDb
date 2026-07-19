// Importa a interface de CatalogStore.
#include "modb/object/catalog_store.hpp"

// Importa o codec de objeto e de valor único.
#include "modb/object/object_codec.hpp"
// Importa BinaryWriter/BinaryReader para o sub-formato dos atributos.
#include "modb/storage/binary.hpp"
// Importa o teto de campos (ADR-007).
#include "modb/limits.hpp"

// Disponibiliza std::min ao limitar reservas.
#include <algorithm>
// Disponibiliza std::as_bytes e std::span ao serializar nomes.
#include <span>
// Disponibiliza std::string ao reconstruir nomes.
#include <string>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {
namespace {

// FieldIds do objeto que representa um TypeDefinition no catálogo.
constexpr FieldId type_name_field{1};
constexpr FieldId type_attributes_field{2};
// FieldId do objeto que representa uma Baseline.
constexpr FieldId baseline_types_field{1};

// Bits do byte de flags de um atributo no sub-formato.
constexpr std::uint8_t flag_nullable = 1U;
constexpr std::uint8_t flag_collection = 2U;
constexpr std::uint8_t flag_embedded = 4U;
constexpr std::uint8_t flag_owned = 8U;

// Localiza um campo pelo id num objeto decodificado.
const AttributeValue* find_field(const DecodedObject& object, FieldId id) {
    for (const auto& [field_id, value] : object.fields) {
        if (field_id == id) {
            return &value;
        }
    }
    return nullptr;
}

// Serializa os atributos de um tipo no sub-formato documentado (Fase 2).
Result<std::vector<std::byte>> encode_attributes(const TypeDefinition& definition) {
    storage::BinaryWriter writer;
    const auto attributes = definition.attributes();
    writer.write_u16(static_cast<std::uint16_t>(attributes.size()));
    for (const auto& attribute : attributes) {
        writer.write_u16(attribute.id.value);
        writer.write_u16(static_cast<std::uint16_t>(attribute.name.size()));
        writer.write_bytes(std::as_bytes(std::span{attribute.name}));
        writer.write_u8(static_cast<std::uint8_t>(attribute.type));
        std::uint8_t flags = 0;
        if (attribute.nullable) {
            flags |= flag_nullable;
        }
        if (attribute.is_collection) {
            flags |= flag_collection;
        }
        if (attribute.is_embedded) {
            flags |= flag_embedded;
        }
        if (attribute.is_owned) {
            flags |= flag_owned;
        }
        writer.write_u8(flags);
        writer.write_u8(attribute.default_value ? 1U : 0U);
        if (attribute.default_value) {
            if (auto result = encode_value(writer, *attribute.default_value); !result) {
                return std::unexpected(result.error());
            }
        }
    }
    return std::move(writer).take();
}

// Reconstrói os atributos a partir do sub-formato (entrada não confiável).
// Mantida no namespace anônimo; a API pública é decode_type_attributes abaixo.
Result<std::vector<AttributeDefinition>> decode_attributes(std::span<const std::byte> bytes) {
    storage::BinaryReader reader{bytes};
    auto count = reader.read_u16();
    if (!count) {
        return std::unexpected(count.error());
    }
    // Rejeita contagens mentirosas antes de reservar ou iterar (Fase 10D).
    if (*count > modb::max_columns_per_table) {
        return std::unexpected(Error{
            ErrorCode::too_many_columns,
            "type attributes exceed max_columns_per_table",
        });
    }
    std::vector<AttributeDefinition> attributes;
    attributes.reserve(*count);
    for (std::uint16_t index = 0; index < *count; ++index) {
        auto field_id = reader.read_u16();
        if (!field_id) {
            return std::unexpected(field_id.error());
        }
        auto name_length = reader.read_u16();
        if (!name_length) {
            return std::unexpected(name_length.error());
        }
        // Nomes acima do teto de identificador são rejeitados sem alocar.
        if (*name_length > modb::max_identifier_bytes) {
            return std::unexpected(Error{
                ErrorCode::invalid_argument,
                "attribute name exceeds max_identifier_bytes",
            });
        }
        auto name_bytes = reader.read_bytes(*name_length);
        if (!name_bytes) {
            return std::unexpected(name_bytes.error());
        }
        auto tag = reader.read_u8();
        if (!tag) {
            return std::unexpected(tag.error());
        }
        auto flags = reader.read_u8();
        if (!flags) {
            return std::unexpected(flags.error());
        }
        auto has_default = reader.read_u8();
        if (!has_default) {
            return std::unexpected(has_default.error());
        }
        AttributeDefinition attribute;
        attribute.id = FieldId{*field_id};
        attribute.name.assign(reinterpret_cast<const char*>(name_bytes->data()),
                              name_bytes->size());
        attribute.type = static_cast<AttributeType>(*tag);
        attribute.nullable = (*flags & flag_nullable) != 0U;
        attribute.is_collection = (*flags & flag_collection) != 0U;
        attribute.is_embedded = (*flags & flag_embedded) != 0U;
        attribute.is_owned = (*flags & flag_owned) != 0U;
        if (*has_default != 0U) {
            auto value = decode_value(reader);
            if (!value) {
                return std::unexpected(value.error());
            }
            attribute.default_value = std::move(*value);
        }
        attributes.push_back(std::move(attribute));
    }
    return attributes;
}

// Decodifica o objeto de catálogo de um TypeDefinition.
Result<DecodedType> decode_type(const DecodedObject& object) {
    const auto* name = find_field(object, type_name_field);
    const auto* attributes_bytes = find_field(object, type_attributes_field);
    if (name == nullptr || attributes_bytes == nullptr) {
        return std::unexpected(
            Error{ErrorCode::corrupt_file, "catalog type object is missing required fields"});
    }
    auto name_view = name->as_string();
    if (!name_view) {
        return std::unexpected(name_view.error());
    }
    auto raw = attributes_bytes->as_bytes();
    if (!raw) {
        return std::unexpected(raw.error());
    }
    auto attributes = decode_attributes(*raw);
    if (!attributes) {
        return std::unexpected(attributes.error());
    }
    // Revalida a definição vinda do disco reutilizando as regras de create.
    auto definition = TypeDefinition::create(std::string{*name_view}, std::move(*attributes));
    if (!definition) {
        return std::unexpected(definition.error());
    }
    return DecodedType{object.id, std::move(*definition)};
}

// Decodifica o objeto de catálogo de uma Baseline.
Result<DecodedBaseline> decode_baseline(const DecodedObject& object) {
    const auto* types = find_field(object, baseline_types_field);
    if (types == nullptr) {
        return std::unexpected(
            Error{ErrorCode::corrupt_file, "catalog baseline object is missing its type list"});
    }
    auto raw = types->as_bytes();
    if (!raw) {
        return std::unexpected(raw.error());
    }
    storage::BinaryReader reader{*raw};
    auto count = reader.read_u16();
    if (!count) {
        return std::unexpected(count.error());
    }
    std::vector<TypeDefinitionId> ids;
    ids.reserve(std::min<std::size_t>(*count, modb::max_columns_per_table));
    for (std::uint16_t index = 0; index < *count; ++index) {
        auto id = reader.read_u64();
        if (!id) {
            return std::unexpected(id.error());
        }
        ids.push_back(TypeDefinitionId{*id});
    }
    auto baseline = Baseline::create(std::move(ids));
    if (!baseline) {
        return std::unexpected(baseline.error());
    }
    return DecodedBaseline{object.id, std::move(*baseline)};
}

} // namespace

Result<std::vector<AttributeDefinition>> decode_type_attributes(std::span<const std::byte> bytes) {
    return decode_attributes(bytes);
}

Result<CatalogStore> CatalogStore::create(storage::PageFile& file) {
    auto heap = storage::TableHeap::create(file);
    if (!heap) {
        return std::unexpected(heap.error());
    }
    return CatalogStore{std::move(*heap)};
}

Result<CatalogStore> CatalogStore::open(storage::PageFile& file, storage::PageId heap_root) {
    auto heap = storage::TableHeap::open(file, heap_root);
    if (!heap) {
        return std::unexpected(heap.error());
    }
    return CatalogStore{std::move(*heap)};
}

Result<void> CatalogStore::save_type(const TypeDefinition& definition) {
    auto attributes = encode_attributes(definition);
    if (!attributes) {
        return std::unexpected(attributes.error());
    }
    FieldValues fields{
        {type_name_field, AttributeValue{definition.name()}},
        {type_attributes_field, AttributeValue{std::move(*attributes)}},
    };
    auto record = encode_object(definition.id(), meta_type_definition, fields);
    if (!record) {
        return std::unexpected(record.error());
    }
    if (auto inserted = heap_.insert(*record); !inserted) {
        return std::unexpected(inserted.error());
    }
    return {};
}

Result<void> CatalogStore::save_baseline(const Baseline& baseline) {
    storage::BinaryWriter writer;
    const auto types = baseline.types();
    writer.write_u16(static_cast<std::uint16_t>(types.size()));
    for (const auto type_id : types) {
        writer.write_u64(type_id.value);
    }
    FieldValues fields{{baseline_types_field, AttributeValue{std::move(writer).take()}}};
    auto record = encode_object(baseline.id(), meta_baseline, fields);
    if (!record) {
        return std::unexpected(record.error());
    }
    if (auto inserted = heap_.insert(*record); !inserted) {
        return std::unexpected(inserted.error());
    }
    return {};
}

Result<CatalogContents> CatalogStore::load_all() {
    auto records = heap_.scan_records();
    if (!records) {
        return std::unexpected(records.error());
    }
    CatalogContents contents;
    for (const auto& record : *records) {
        auto object = decode_object(record.bytes);
        if (!object) {
            return std::unexpected(object.error());
        }
        if (object->type == meta_type_definition) {
            auto type = decode_type(*object);
            if (!type) {
                return std::unexpected(type.error());
            }
            contents.types.push_back(std::move(*type));
        } else if (object->type == meta_baseline) {
            auto baseline = decode_baseline(*object);
            if (!baseline) {
                return std::unexpected(baseline.error());
            }
            contents.baselines.push_back(std::move(*baseline));
        } else {
            return std::unexpected(Error{ErrorCode::corrupt_file,
                                         "catalog heap contains an unknown meta-type object"});
        }
    }
    return contents;
}

} // namespace modb::object
