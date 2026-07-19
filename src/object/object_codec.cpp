// Importa a interface do codec.
#include "modb/object/object_codec.hpp"

// Importa BinaryWriter/BinaryReader, reaproveitados como base little-endian.
#include "modb/storage/binary.hpp"
// Importa os limites compartilhados (teto de campos, ADR-007).
#include "modb/limits.hpp"
// Importa o helper de visitação exaustiva de variant.
#include "modb/detail/overloaded.hpp"

// Disponibiliza std::min ao limitar a reserva de campos.
#include <algorithm>
// Disponibiliza std::bit_cast para int64/float64 <-> u64 sem UB.
#include <bit>
// Disponibiliza std::numeric_limits ao guardar comprimentos.
#include <limits>
// Disponibiliza std::as_bytes e std::span ao serializar texto.
#include <span>
// Disponibiliza std::string ao reconstruir textos.
#include <string>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {

// Escreve tag (u8) + valor no encoding do ADR-003. Guarda o comprimento de
// string/bytes contra o limite de u32 do formato.
Result<void> encode_value(storage::BinaryWriter& writer, const AttributeValue& value) {
    return std::visit(
        detail::Overloaded{
            [&](AttributeNull) -> Result<void> {
                writer.write_u8(static_cast<std::uint8_t>(AttributeType::null));
                return {};
            },
            [&](bool boolean) -> Result<void> {
                writer.write_u8(static_cast<std::uint8_t>(AttributeType::boolean));
                writer.write_u8(boolean ? 1U : 0U);
                return {};
            },
            [&](std::int64_t integer) -> Result<void> {
                writer.write_u8(static_cast<std::uint8_t>(AttributeType::int64));
                writer.write_u64(std::bit_cast<std::uint64_t>(integer));
                return {};
            },
            [&](double real) -> Result<void> {
                writer.write_u8(static_cast<std::uint8_t>(AttributeType::float64));
                writer.write_u64(std::bit_cast<std::uint64_t>(real));
                return {};
            },
            [&](const std::string& text) -> Result<void> {
                if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected(
                        Error{ErrorCode::value_too_large, "string is too large to encode"});
                }
                writer.write_u8(static_cast<std::uint8_t>(AttributeType::string));
                writer.write_u32(static_cast<std::uint32_t>(text.size()));
                writer.write_bytes(std::as_bytes(std::span{text}));
                return {};
            },
            [&](const std::vector<std::byte>& bytes) -> Result<void> {
                if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected(
                        Error{ErrorCode::value_too_large, "byte string is too large to encode"});
                }
                writer.write_u8(static_cast<std::uint8_t>(AttributeType::bytes));
                writer.write_u32(static_cast<std::uint32_t>(bytes.size()));
                writer.write_bytes(bytes);
                return {};
            },
            [&](ObjectId ref) -> Result<void> {
                writer.write_u8(static_cast<std::uint8_t>(AttributeType::ref));
                writer.write_u64(ref.value);
                return {};
            },
            [&](BlobId blob) -> Result<void> {
                writer.write_u8(static_cast<std::uint8_t>(AttributeType::blob));
                writer.write_u64(blob.value);
                return {};
            },
            [&](const EmbeddedValue& embedded) -> Result<void> {
                if (embedded.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected(
                        Error{ErrorCode::value_too_large, "embedded object is too large to encode"});
                }
                writer.write_u8(static_cast<std::uint8_t>(AttributeType::embedded));
                writer.write_u32(static_cast<std::uint32_t>(embedded.payload.size()));
                writer.write_bytes(embedded.payload);
                return {};
            },
        },
        value.storage());
}

// Lê tag (u8) + valor. Cada comprimento é validado por read_bytes contra os
// bytes restantes antes de qualquer alocação.
Result<AttributeValue> decode_value(storage::BinaryReader& reader) {
    auto tag = reader.read_u8();
    if (!tag) {
        return std::unexpected(tag.error());
    }
    switch (static_cast<AttributeType>(*tag)) {
    case AttributeType::null:
        return AttributeValue{AttributeNull{}};
    case AttributeType::boolean: {
        auto raw = reader.read_u8();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        if (*raw > 1U) {
            return std::unexpected(
                Error{ErrorCode::invalid_encoding, "BOOLEAN must be encoded as 0 or 1"});
        }
        return AttributeValue{*raw != 0U};
    }
    case AttributeType::int64: {
        auto raw = reader.read_u64();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        return AttributeValue{std::bit_cast<std::int64_t>(*raw)};
    }
    case AttributeType::float64: {
        auto raw = reader.read_u64();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        return AttributeValue{std::bit_cast<double>(*raw)};
    }
    case AttributeType::string: {
        auto length = reader.read_u32();
        if (!length) {
            return std::unexpected(length.error());
        }
        auto data = reader.read_bytes(*length);
        if (!data) {
            return std::unexpected(data.error());
        }
        return AttributeValue{
            std::string{reinterpret_cast<const char*>(data->data()), data->size()}};
    }
    case AttributeType::bytes: {
        auto length = reader.read_u32();
        if (!length) {
            return std::unexpected(length.error());
        }
        auto data = reader.read_bytes(*length);
        if (!data) {
            return std::unexpected(data.error());
        }
        return AttributeValue{std::vector<std::byte>{data->begin(), data->end()}};
    }
    case AttributeType::ref: {
        auto raw = reader.read_u64();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        return AttributeValue{ObjectId{*raw}};
    }
    case AttributeType::blob: {
        auto raw = reader.read_u64();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        return AttributeValue{BlobId{*raw}};
    }
    case AttributeType::embedded: {
        auto length = reader.read_u32();
        if (!length) {
            return std::unexpected(length.error());
        }
        auto data = reader.read_bytes(*length);
        if (!data) {
            return std::unexpected(data.error());
        }
        return AttributeValue{EmbeddedValue{std::vector<std::byte>{data->begin(), data->end()}}};
    }
    }
    return std::unexpected(Error{ErrorCode::invalid_encoding, "unknown attribute tag"});
}

Result<std::vector<std::byte>> encode_object_payload(const FieldValues& fields) {
    // O teto de campos por objeto é o mesmo de atributos por tipo (ADR-007).
    if (fields.size() > modb::max_columns_per_table) {
        return std::unexpected(
            Error{ErrorCode::too_many_columns, "object has more fields than the type limit"});
    }

    storage::BinaryWriter writer;
    writer.write_u8(object_payload_version);
    writer.write_u16(static_cast<std::uint16_t>(fields.size()));
    for (const auto& [field_id, value] : fields) {
        writer.write_u16(field_id.value);
        if (auto result = encode_value(writer, value); !result) {
            return std::unexpected(result.error());
        }
    }
    return std::move(writer).take();
}

Result<FieldValues> decode_object_payload(std::span<const std::byte> payload) {
    storage::BinaryReader reader{payload};
    auto version = reader.read_u8();
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != object_payload_version) {
        return std::unexpected(Error{
            ErrorCode::invalid_encoding,
            "unsupported object payload version: " + std::to_string(*version)});
    }
    auto field_count = reader.read_u16();
    if (!field_count) {
        return std::unexpected(field_count.error());
    }
    // Contagem acima do teto ADR-007: erro antes de alocar/iterar (Fase 10D).
    if (*field_count > modb::max_columns_per_table) {
        return std::unexpected(
            Error{ErrorCode::too_many_columns, "object has more fields than the type limit"});
    }

    FieldValues fields;
    fields.reserve(*field_count);
    for (std::uint16_t index = 0; index < *field_count; ++index) {
        auto field_id = reader.read_u16();
        if (!field_id) {
            return std::unexpected(field_id.error());
        }
        for (const auto& [seen_id, seen_value] : fields) {
            if (seen_id.value == *field_id) {
                return std::unexpected(Error{
                    ErrorCode::invalid_encoding,
                    "duplicate FieldId " + std::to_string(*field_id) + " in object payload"});
            }
        }
        auto value = decode_value(reader);
        if (!value) {
            return std::unexpected(value.error());
        }
        fields.emplace_back(FieldId{*field_id}, std::move(*value));
    }
    if (!reader.at_end()) {
        return std::unexpected(
            Error{ErrorCode::trailing_data, "object payload has trailing bytes"});
    }
    return fields;
}

Result<std::vector<std::byte>> encode_object(ObjectId id, TypeDefinitionId type,
                                             const FieldValues& fields) {
    auto payload = encode_object_payload(fields);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    storage::BinaryWriter writer;
    writer.write_u64(id.value);
    writer.write_u64(type.value);
    writer.write_bytes(*payload);
    return std::move(writer).take();
}

Result<DecodedObject> decode_object(std::span<const std::byte> record) {
    storage::BinaryReader reader{record};

    auto id = reader.read_u64();
    if (!id) {
        return std::unexpected(id.error());
    }
    auto type = reader.read_u64();
    if (!type) {
        return std::unexpected(type.error());
    }
    auto fields = decode_object_payload(reader.remaining_bytes());
    if (!fields) {
        return std::unexpected(fields.error());
    }
    return DecodedObject{ObjectId{*id}, TypeDefinitionId{*type}, std::move(*fields)};
}

Result<ObjectHeaderView> decode_object_header(std::span<const std::byte> record) {
    storage::BinaryReader reader{record};
    auto id = reader.read_u64();
    if (!id) {
        return std::unexpected(id.error());
    }
    auto type = reader.read_u64();
    if (!type) {
        return std::unexpected(type.error());
    }
    return ObjectHeaderView{ObjectId{*id}, TypeDefinitionId{*type}};
}

} // namespace modb::object
