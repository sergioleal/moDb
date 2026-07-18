#include "modb/net/protocol.hpp"

#include "modb/object/object_codec.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace modb::net {
namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{.code = code, .message = std::move(message)};
}

Result<void> write_string(storage::BinaryWriter& writer, std::string_view text) {
    if (text.size() > max_string_bytes) {
        return std::unexpected(make_error(ErrorCode::value_too_large,
                                          "protocol string exceeds max_string_bytes"));
    }
    writer.write_u32(static_cast<std::uint32_t>(text.size()));
    const auto bytes =
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()), text.size()};
    writer.write_bytes(bytes);
    return {};
}

Result<std::string> read_string(storage::BinaryReader& reader) {
    const auto length = reader.read_u32();
    if (!length) {
        return std::unexpected(length.error());
    }
    if (*length > max_string_bytes) {
        return std::unexpected(
            make_error(ErrorCode::protocol_error, "protocol string length exceeds limit"));
    }
    if (reader.remaining() < *length) {
        return std::unexpected(
            make_error(ErrorCode::unexpected_end_of_input, "truncated bytes truncated"));
    }
    auto bytes = reader.read_bytes(*length);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    std::string text(bytes->size(), '\0');
    if (!bytes->empty()) {
        std::memcpy(text.data(), bytes->data(), bytes->size());
    }
    return text;
}

Result<void> write_compression_list(storage::BinaryWriter& writer,
                                    const std::vector<Compression>& codecs) {
    if (codecs.size() > 255) {
        return std::unexpected(
            make_error(ErrorCode::value_too_large, "too many compression codecs"));
    }
    writer.write_u8(static_cast<std::uint8_t>(codecs.size()));
    for (const Compression codec : codecs) {
        writer.write_u8(static_cast<std::uint8_t>(codec));
    }
    return {};
}

Result<std::vector<Compression>> read_compression_list(storage::BinaryReader& reader) {
    const auto count = reader.read_u8();
    if (!count) {
        return std::unexpected(count.error());
    }
    if (reader.remaining() < *count) {
        return std::unexpected(make_error(ErrorCode::unexpected_end_of_input,
                                          "compression list truncated"));
    }
    std::vector<Compression> codecs;
    codecs.reserve(*count);
    for (std::uint8_t i = 0; i < *count; ++i) {
        const auto raw = reader.read_u8();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        codecs.push_back(static_cast<Compression>(*raw));
    }
    return codecs;
}

Result<void> encode_hello(storage::BinaryWriter& writer, const Hello& message) {
    writer.write_u16(message.version);
    if (auto status = write_string(writer, message.database_name); !status) {
        return status;
    }
    return write_compression_list(writer, message.accepted_codecs);
}

Result<Hello> decode_hello(storage::BinaryReader& reader) {
    Hello message;
    const auto version = reader.read_u16();
    if (!version) {
        return std::unexpected(version.error());
    }
    message.version = *version;
    auto name = read_string(reader);
    if (!name) {
        return std::unexpected(name.error());
    }
    message.database_name = std::move(*name);
    auto codecs = read_compression_list(reader);
    if (!codecs) {
        return std::unexpected(codecs.error());
    }
    message.accepted_codecs = std::move(*codecs);
    return message;
}

Result<void> encode_hello_ok(storage::BinaryWriter& writer, const HelloOk& message) {
    writer.write_u16(message.version);
    writer.write_u64(message.baseline.value);
    writer.write_u8(static_cast<std::uint8_t>(message.selected_codec));
    writer.write_u32(message.max_frame_bytes);
    return {};
}

Result<HelloOk> decode_hello_ok(storage::BinaryReader& reader) {
    HelloOk message;
    const auto version = reader.read_u16();
    if (!version) {
        return std::unexpected(version.error());
    }
    message.version = *version;
    const auto baseline = reader.read_u64();
    if (!baseline) {
        return std::unexpected(baseline.error());
    }
    message.baseline = object::BaselineId{*baseline};
    const auto codec = reader.read_u8();
    if (!codec) {
        return std::unexpected(codec.error());
    }
    message.selected_codec = static_cast<Compression>(*codec);
    const auto max_frame = reader.read_u32();
    if (!max_frame) {
        return std::unexpected(max_frame.error());
    }
    message.max_frame_bytes = *max_frame;
    return message;
}

Result<void> encode_query(storage::BinaryWriter& writer, const Query& message) {
    writer.write_u32(message.query_id);
    return encode_query_description(writer, message.description);
}

Result<Query> decode_query(storage::BinaryReader& reader) {
    Query message;
    const auto query_id = reader.read_u32();
    if (!query_id) {
        return std::unexpected(query_id.error());
    }
    message.query_id = *query_id;
    auto description = decode_query_description(reader);
    if (!description) {
        return std::unexpected(description.error());
    }
    message.description = std::move(*description);
    return message;
}

Result<void> encode_stream_begin(storage::BinaryWriter& writer, const StreamBegin& message) {
    writer.write_u32(message.query_id);
    return {};
}

Result<StreamBegin> decode_stream_begin(storage::BinaryReader& reader) {
    const auto query_id = reader.read_u32();
    if (!query_id) {
        return std::unexpected(query_id.error());
    }
    return StreamBegin{.query_id = *query_id};
}

Result<void> encode_stream_end(storage::BinaryWriter& writer, const StreamEnd& message) {
    writer.write_u32(message.query_id);
    writer.write_u64(message.total);
    return {};
}

Result<StreamEnd> decode_stream_end(storage::BinaryReader& reader) {
    StreamEnd message;
    const auto query_id = reader.read_u32();
    if (!query_id) {
        return std::unexpected(query_id.error());
    }
    message.query_id = *query_id;
    const auto total = reader.read_u64();
    if (!total) {
        return std::unexpected(total.error());
    }
    message.total = *total;
    return message;
}

Result<void> encode_stream_error(storage::BinaryWriter& writer, const StreamError& message) {
    writer.write_u32(message.query_id);
    writer.write_u16(static_cast<std::uint16_t>(message.code));
    return write_string(writer, message.message);
}

Result<StreamError> decode_stream_error(storage::BinaryReader& reader) {
    StreamError message;
    const auto query_id = reader.read_u32();
    if (!query_id) {
        return std::unexpected(query_id.error());
    }
    message.query_id = *query_id;
    const auto code = reader.read_u16();
    if (!code) {
        return std::unexpected(code.error());
    }
    message.code = static_cast<ErrorCode>(*code);
    auto text = read_string(reader);
    if (!text) {
        return std::unexpected(text.error());
    }
    message.message = std::move(*text);
    return message;
}

Result<void> encode_cancel(storage::BinaryWriter& writer, const Cancel& message) {
    writer.write_u32(message.query_id);
    return {};
}

Result<Cancel> decode_cancel(storage::BinaryReader& reader) {
    const auto query_id = reader.read_u32();
    if (!query_id) {
        return std::unexpected(query_id.error());
    }
    return Cancel{.query_id = *query_id};
}

Result<void> encode_object_frame_payload(storage::BinaryWriter& writer,
                                         const ObjectFrame& frame) {
    if (frame.compression != Compression::none) {
        return std::unexpected(make_error(
            ErrorCode::protocol_error, "Fase 8A only encodes ObjectFrame with compression=none"));
    }

    storage::BinaryWriter data;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> slots;
    slots.reserve(frame.records.size());
    for (const ObjectEnvelope& envelope : frame.records) {
        const auto offset = static_cast<std::uint32_t>(data.bytes().size());
        if (auto status = encode_object_envelope(data, envelope); !status) {
            return status;
        }
        const auto length = static_cast<std::uint32_t>(data.bytes().size() - offset);
        slots.emplace_back(offset, length);
    }

    if (data.bytes().size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(
            make_error(ErrorCode::value_too_large, "ObjectFrame uncompressed data too large"));
    }
    if (slots.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(
            make_error(ErrorCode::value_too_large, "ObjectFrame record_count too large"));
    }

    const auto uncompressed_size = static_cast<std::uint32_t>(data.bytes().size());
    writer.write_u32(frame.query_id);
    writer.write_u32(static_cast<std::uint32_t>(slots.size()));
    writer.write_u8(static_cast<std::uint8_t>(Compression::none));
    writer.write_u8(0);
    writer.write_u8(0);
    writer.write_u8(0);
    writer.write_u32(uncompressed_size);
    writer.write_u32(uncompressed_size); // encoded_size == uncompressed_size for none
    for (const auto& [offset, length] : slots) {
        writer.write_u32(offset);
        writer.write_u32(length);
    }
    writer.write_bytes(data.bytes());
    return {};
}

Result<void> validate_slot_directory(
    std::uint32_t uncompressed_size,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& slots) {
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto [offset, length] = slots[i];
        if (length == 0) {
            return std::unexpected(
                make_error(ErrorCode::protocol_error, "ObjectFrame slot length is zero"));
        }
        if (offset > uncompressed_size || length > uncompressed_size - offset) {
            return std::unexpected(
                make_error(ErrorCode::protocol_error, "ObjectFrame slot outside data area"));
        }
        const auto end = offset + length;
        for (std::size_t j = 0; j < i; ++j) {
            const auto [other_offset, other_length] = slots[j];
            const auto other_end = other_offset + other_length;
            if (offset < other_end && other_offset < end) {
                return std::unexpected(
                    make_error(ErrorCode::protocol_error, "ObjectFrame slots overlap"));
            }
        }
    }
    return {};
}

Result<ObjectFrame> decode_object_frame_payload(storage::BinaryReader& reader) {
    ObjectFrame frame;
    const auto query_id = reader.read_u32();
    if (!query_id) {
        return std::unexpected(query_id.error());
    }
    frame.query_id = *query_id;

    const auto record_count = reader.read_u32();
    if (!record_count) {
        return std::unexpected(record_count.error());
    }

    const auto compression = reader.read_u8();
    if (!compression) {
        return std::unexpected(compression.error());
    }
    frame.compression = static_cast<Compression>(*compression);

    // reservado[3]
    for (int i = 0; i < 3; ++i) {
        if (auto reserved = reader.read_u8(); !reserved) {
            return std::unexpected(reserved.error());
        }
    }

    const auto uncompressed_size = reader.read_u32();
    if (!uncompressed_size) {
        return std::unexpected(uncompressed_size.error());
    }
    const auto encoded_size = reader.read_u32();
    if (!encoded_size) {
        return std::unexpected(encoded_size.error());
    }

    if (frame.compression != Compression::none) {
        return std::unexpected(make_error(ErrorCode::protocol_error,
                                          "Fase 8A only accepts compression=none"));
    }
    if (*encoded_size != *uncompressed_size) {
        return std::unexpected(make_error(
            ErrorCode::protocol_error,
            "compression=none requires encoded_size == uncompressed_size"));
    }

    const auto directory_bytes =
        static_cast<std::uint64_t>(*record_count) * 8u; // offset u32 + length u32
    if (directory_bytes > reader.remaining()) {
        return std::unexpected(
            make_error(ErrorCode::protocol_error, "ObjectFrame directory truncated"));
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> slots;
    slots.reserve(*record_count);
    for (std::uint32_t i = 0; i < *record_count; ++i) {
        const auto offset = reader.read_u32();
        if (!offset) {
            return std::unexpected(offset.error());
        }
        const auto length = reader.read_u32();
        if (!length) {
            return std::unexpected(length.error());
        }
        slots.emplace_back(*offset, *length);
    }

    if (auto status = validate_slot_directory(*uncompressed_size, slots); !status) {
        return std::unexpected(status.error());
    }

    if (reader.remaining() < *encoded_size) {
        return std::unexpected(
            make_error(ErrorCode::unexpected_end_of_input, "ObjectFrame data truncated"));
    }
    auto encoded = reader.read_bytes(*encoded_size);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }

    frame.records.reserve(slots.size());
    for (const auto& [offset, length] : slots) {
        const auto slice = encoded->subspan(offset, length);
        storage::BinaryReader envelope_reader{slice};
        auto envelope = decode_object_envelope(envelope_reader);
        if (!envelope) {
            return std::unexpected(envelope.error());
        }
        if (!envelope_reader.at_end()) {
            return std::unexpected(make_error(ErrorCode::protocol_error,
                                              "ObjectEnvelope has trailing bytes in slot"));
        }
        frame.records.push_back(std::move(*envelope));
    }
    return frame;
}

Result<void> encode_payload(storage::BinaryWriter& writer, const Message& message) {
    return std::visit(
        [&writer](const auto& body) -> Result<void> {
            using T = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<T, Hello>) {
                return encode_hello(writer, body);
            } else if constexpr (std::is_same_v<T, HelloOk>) {
                return encode_hello_ok(writer, body);
            } else if constexpr (std::is_same_v<T, Query>) {
                return encode_query(writer, body);
            } else if constexpr (std::is_same_v<T, StreamBegin>) {
                return encode_stream_begin(writer, body);
            } else if constexpr (std::is_same_v<T, ObjectFrame>) {
                return encode_object_frame_payload(writer, body);
            } else if constexpr (std::is_same_v<T, StreamEnd>) {
                return encode_stream_end(writer, body);
            } else if constexpr (std::is_same_v<T, StreamError>) {
                return encode_stream_error(writer, body);
            } else if constexpr (std::is_same_v<T, Cancel>) {
                return encode_cancel(writer, body);
            }
        },
        message);
}

Result<Message> decode_payload(MessageType type, storage::BinaryReader& reader) {
    switch (type) {
    case MessageType::hello: {
        auto body = decode_hello(reader);
        if (!body) {
            return std::unexpected(body.error());
        }
        return Message{std::move(*body)};
    }
    case MessageType::hello_ok: {
        auto body = decode_hello_ok(reader);
        if (!body) {
            return std::unexpected(body.error());
        }
        return Message{std::move(*body)};
    }
    case MessageType::query: {
        auto body = decode_query(reader);
        if (!body) {
            return std::unexpected(body.error());
        }
        return Message{std::move(*body)};
    }
    case MessageType::stream_begin: {
        auto body = decode_stream_begin(reader);
        if (!body) {
            return std::unexpected(body.error());
        }
        return Message{*body};
    }
    case MessageType::object_frame: {
        auto body = decode_object_frame_payload(reader);
        if (!body) {
            return std::unexpected(body.error());
        }
        return Message{std::move(*body)};
    }
    case MessageType::stream_end: {
        auto body = decode_stream_end(reader);
        if (!body) {
            return std::unexpected(body.error());
        }
        return Message{*body};
    }
    case MessageType::stream_error: {
        auto body = decode_stream_error(reader);
        if (!body) {
            return std::unexpected(body.error());
        }
        return Message{std::move(*body)};
    }
    case MessageType::cancel: {
        auto body = decode_cancel(reader);
        if (!body) {
            return std::unexpected(body.error());
        }
        return Message{*body};
    }
    }
    return std::unexpected(make_error(ErrorCode::protocol_error, "unknown message type"));
}

} // namespace

Result<void> encode_query_description(storage::BinaryWriter& writer,
                                      const QueryDescription& description) {
    writer.write_u64(description.type.value);
    writer.write_u64(description.limit);
    writer.write_u8(description.equals ? 1U : 0U);
    if (description.equals) {
        writer.write_u16(description.equals->field.value);
        if (auto status = object::encode_value(writer, description.equals->value); !status) {
            return status;
        }
    }
    if (description.project.size() > (std::numeric_limits<std::uint16_t>::max)()) {
        return std::unexpected(
            make_error(ErrorCode::value_too_large, "QueryDescription project list too large"));
    }
    writer.write_u16(static_cast<std::uint16_t>(description.project.size()));
    for (const object::FieldId field : description.project) {
        writer.write_u16(field.value);
    }
    return {};
}

Result<QueryDescription> decode_query_description(storage::BinaryReader& reader) {
    QueryDescription description;
    const auto type = reader.read_u64();
    if (!type) {
        return std::unexpected(type.error());
    }
    description.type = object::TypeDefinitionId{*type};
    const auto limit = reader.read_u64();
    if (!limit) {
        return std::unexpected(limit.error());
    }
    description.limit = *limit;
    const auto has_equals = reader.read_u8();
    if (!has_equals) {
        return std::unexpected(has_equals.error());
    }
    if (*has_equals > 1) {
        return std::unexpected(
            make_error(ErrorCode::protocol_error, "QueryDescription has_equals must be 0 or 1"));
    }
    if (*has_equals == 1) {
        EqualityFilter filter;
        const auto field = reader.read_u16();
        if (!field) {
            return std::unexpected(field.error());
        }
        filter.field = object::FieldId{*field};
        auto value = object::decode_value(reader);
        if (!value) {
            return std::unexpected(value.error());
        }
        filter.value = std::move(*value);
        description.equals = std::move(filter);
    }
    const auto project_count = reader.read_u16();
    if (!project_count) {
        return std::unexpected(project_count.error());
    }
    if (reader.remaining() < static_cast<std::size_t>(*project_count) * 2u) {
        return std::unexpected(
            make_error(ErrorCode::unexpected_end_of_input, "QueryDescription project truncated"));
    }
    description.project.reserve(*project_count);
    for (std::uint16_t i = 0; i < *project_count; ++i) {
        const auto field = reader.read_u16();
        if (!field) {
            return std::unexpected(field.error());
        }
        description.project.push_back(object::FieldId{*field});
    }
    return description;
}

MessageType message_type(const Message& message) noexcept {
    return std::visit(
        [](const auto& body) -> MessageType {
            using T = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<T, Hello>) {
                return MessageType::hello;
            } else if constexpr (std::is_same_v<T, HelloOk>) {
                return MessageType::hello_ok;
            } else if constexpr (std::is_same_v<T, Query>) {
                return MessageType::query;
            } else if constexpr (std::is_same_v<T, StreamBegin>) {
                return MessageType::stream_begin;
            } else if constexpr (std::is_same_v<T, ObjectFrame>) {
                return MessageType::object_frame;
            } else if constexpr (std::is_same_v<T, StreamEnd>) {
                return MessageType::stream_end;
            } else if constexpr (std::is_same_v<T, StreamError>) {
                return MessageType::stream_error;
            } else if constexpr (std::is_same_v<T, Cancel>) {
                return MessageType::cancel;
            }
        },
        message);
}

Result<void> encode_object_envelope(storage::BinaryWriter& writer,
                                    const ObjectEnvelope& envelope) {
    if (envelope.payload.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(
            make_error(ErrorCode::value_too_large, "ObjectEnvelope payload too large"));
    }
    writer.write_u64(envelope.object_id.value);
    writer.write_u64(envelope.type_definition_id.value);
    writer.write_u32(static_cast<std::uint32_t>(envelope.payload.size()));
    writer.write_bytes(envelope.payload);
    return {};
}

Result<ObjectEnvelope> decode_object_envelope(storage::BinaryReader& reader) {
    ObjectEnvelope envelope;
    const auto object_id = reader.read_u64();
    if (!object_id) {
        return std::unexpected(object_id.error());
    }
    envelope.object_id = object::ObjectId{*object_id};
    const auto type_id = reader.read_u64();
    if (!type_id) {
        return std::unexpected(type_id.error());
    }
    envelope.type_definition_id = object::TypeDefinitionId{*type_id};
    const auto payload_length = reader.read_u32();
    if (!payload_length) {
        return std::unexpected(payload_length.error());
    }
    if (reader.remaining() < *payload_length) {
        return std::unexpected(
            make_error(ErrorCode::unexpected_end_of_input, "ObjectEnvelope payload truncated"));
    }
    auto payload = reader.read_bytes(*payload_length);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    envelope.payload.assign(payload->begin(), payload->end());
    return envelope;
}

Result<std::vector<std::byte>> encode_message(const Message& message) {
    storage::BinaryWriter payload;
    if (auto status = encode_payload(payload, message); !status) {
        return std::unexpected(status.error());
    }

    const auto type = static_cast<std::uint8_t>(message_type(message));
    const auto payload_size = payload.bytes().size();
    if (payload_size > max_frame_bytes - 1u) {
        return std::unexpected(
            make_error(ErrorCode::frame_too_large, "encoded message exceeds max_frame_bytes"));
    }
    const auto length = static_cast<std::uint32_t>(1u + payload_size);

    storage::BinaryWriter frame;
    frame.write_u32(length);
    frame.write_u8(type);
    frame.write_bytes(payload.bytes());
    return std::move(frame).take();
}

Result<Message> decode_message(std::span<const std::byte> bytes) {
    if (bytes.size() < 4) {
        return std::unexpected(
            make_error(ErrorCode::unexpected_end_of_input, "frame length truncated"));
    }

    storage::BinaryReader header{bytes.subspan(0, 4)};
    const auto length = header.read_u32();
    if (!length) {
        return std::unexpected(length.error());
    }
    if (*length > max_frame_bytes) {
        return std::unexpected(
            make_error(ErrorCode::frame_too_large, "frame length exceeds 16 MiB"));
    }
    if (*length == 0) {
        return std::unexpected(make_error(ErrorCode::protocol_error, "frame length is zero"));
    }
    if (bytes.size() < 4u + *length) {
        return std::unexpected(
            make_error(ErrorCode::unexpected_end_of_input, "frame payload truncated"));
    }
    if (bytes.size() > 4u + *length) {
        return std::unexpected(
            make_error(ErrorCode::trailing_data, "bytes remain after complete frame"));
    }

    storage::BinaryReader body{bytes.subspan(4, *length)};
    const auto type_raw = body.read_u8();
    if (!type_raw) {
        return std::unexpected(type_raw.error());
    }
    const auto type = static_cast<MessageType>(*type_raw);
    auto message = decode_payload(type, body);
    if (!message) {
        return message;
    }
    if (!body.at_end()) {
        return std::unexpected(
            make_error(ErrorCode::trailing_data, "bytes remain after message payload"));
    }
    return message;
}

} // namespace modb::net
