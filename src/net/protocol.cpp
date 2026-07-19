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
    writer.write_u16(message.max_concurrent_streams);
    writer.write_u16(message.max_expansion_ratio);
    writer.write_u32(message.idle_timeout_ms);
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
    if (!is_known_compression(message.selected_codec)) {
        return std::unexpected(
            make_error(ErrorCode::protocol_error, "HelloOk selected unknown compression codec"));
    }
    const auto max_frame = reader.read_u32();
    if (!max_frame) {
        return std::unexpected(max_frame.error());
    }
    message.max_frame_bytes = *max_frame;
    const auto max_streams = reader.read_u16();
    if (!max_streams) {
        return std::unexpected(max_streams.error());
    }
    message.max_concurrent_streams = *max_streams;
    const auto expansion = reader.read_u16();
    if (!expansion) {
        return std::unexpected(expansion.error());
    }
    if (*expansion == 0) {
        return std::unexpected(
            make_error(ErrorCode::protocol_error, "HelloOk max_expansion_ratio must be >= 1"));
    }
    message.max_expansion_ratio = *expansion;
    const auto idle = reader.read_u32();
    if (!idle) {
        return std::unexpected(idle.error());
    }
    message.idle_timeout_ms = *idle;
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

Result<void> encode_op_call(storage::BinaryWriter& writer, const OpCall& message) {
    writer.write_u32(message.call_id);
    if (auto status = write_string(writer, message.operation_id); !status) {
        return status;
    }
    if (message.args.size() > max_frame_bytes) {
        return std::unexpected(
            make_error(ErrorCode::value_too_large, "OpCall args exceed max_frame_bytes"));
    }
    writer.write_u32(static_cast<std::uint32_t>(message.args.size()));
    writer.write_bytes(message.args);
    return {};
}

Result<OpCall> decode_op_call(storage::BinaryReader& reader) {
    OpCall message;
    const auto call_id = reader.read_u32();
    if (!call_id) {
        return std::unexpected(call_id.error());
    }
    message.call_id = *call_id;
    auto op_id = read_string(reader);
    if (!op_id) {
        return std::unexpected(op_id.error());
    }
    message.operation_id = std::move(*op_id);
    const auto args_len = reader.read_u32();
    if (!args_len) {
        return std::unexpected(args_len.error());
    }
    if (reader.remaining() < *args_len) {
        return std::unexpected(
            make_error(ErrorCode::unexpected_end_of_input, "OpCall args truncated"));
    }
    auto args = reader.read_bytes(*args_len);
    if (!args) {
        return std::unexpected(args.error());
    }
    message.args.assign(args->begin(), args->end());
    return message;
}

Result<void> encode_op_result(storage::BinaryWriter& writer, const OpResult& message) {
    writer.write_u32(message.call_id);
    writer.write_u8(message.ok ? 1U : 0U);
    if (!message.ok) {
        writer.write_u16(static_cast<std::uint16_t>(message.code));
        if (auto status = write_string(writer, message.message); !status) {
            return status;
        }
        return {};
    }
    if (message.payload.size() > max_frame_bytes) {
        return std::unexpected(
            make_error(ErrorCode::value_too_large, "OpResult payload exceeds max_frame_bytes"));
    }
    writer.write_u32(static_cast<std::uint32_t>(message.payload.size()));
    writer.write_bytes(message.payload);
    return {};
}

Result<OpResult> decode_op_result(storage::BinaryReader& reader) {
    OpResult message;
    const auto call_id = reader.read_u32();
    if (!call_id) {
        return std::unexpected(call_id.error());
    }
    message.call_id = *call_id;
    const auto ok = reader.read_u8();
    if (!ok) {
        return std::unexpected(ok.error());
    }
    if (*ok > 1) {
        return std::unexpected(make_error(ErrorCode::protocol_error, "OpResult ok must be 0 or 1"));
    }
    message.ok = (*ok == 1);
    if (!message.ok) {
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
    const auto payload_len = reader.read_u32();
    if (!payload_len) {
        return std::unexpected(payload_len.error());
    }
    if (reader.remaining() < *payload_len) {
        return std::unexpected(
            make_error(ErrorCode::unexpected_end_of_input, "OpResult payload truncated"));
    }
    auto payload = reader.read_bytes(*payload_len);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    message.payload.assign(payload->begin(), payload->end());
    return message;
}

[[nodiscard]] bool materially_smaller(std::size_t encoded, std::size_t uncompressed) noexcept {
    // Exige pelo menos ~12.5% de redução (encoded < uncompressed * 7/8).
    return encoded < (uncompressed * 7u) / 8u;
}

Result<void> encode_object_frame_payload(storage::BinaryWriter& writer,
                                         const ObjectFrame& frame) {
    if (!is_known_compression(frame.compression)) {
        return std::unexpected(
            make_error(ErrorCode::protocol_error, "ObjectFrame compression codec is unknown"));
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
    Compression chosen = Compression::none;
    std::span<const std::byte> encoded_view = data.bytes();
    std::vector<std::byte> compressed;

    if (frame.compression == Compression::rle && uncompressed_size >= compression_min_bytes) {
        auto tried = compress_rle(data.bytes());
        if (tried && materially_smaller(tried->size(), uncompressed_size)) {
            compressed = std::move(*tried);
            encoded_view = compressed;
            chosen = Compression::rle;
        }
    }

    if (encoded_view.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(
            make_error(ErrorCode::value_too_large, "ObjectFrame encoded data too large"));
    }
    const auto encoded_size = static_cast<std::uint32_t>(encoded_view.size());

    writer.write_u32(frame.query_id);
    writer.write_u32(static_cast<std::uint32_t>(slots.size()));
    writer.write_u8(static_cast<std::uint8_t>(chosen));
    writer.write_u8(0);
    writer.write_u8(0);
    writer.write_u8(0);
    writer.write_u32(uncompressed_size);
    writer.write_u32(encoded_size);
    for (const auto& [offset, length] : slots) {
        writer.write_u32(offset);
        writer.write_u32(length);
    }
    writer.write_bytes(encoded_view);
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

Result<ObjectFrame> decode_object_frame_payload(storage::BinaryReader& reader,
                                                std::uint32_t negotiated_max_frame,
                                                std::uint16_t max_expansion_ratio) {
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

    if (!is_known_compression(frame.compression)) {
        return std::unexpected(
            make_error(ErrorCode::protocol_error, "ObjectFrame compression codec is unknown"));
    }
    if (*uncompressed_size > negotiated_max_frame) {
        return std::unexpected(make_error(ErrorCode::frame_too_large,
                                          "ObjectFrame uncompressed_size exceeds negotiated max"));
    }
    if (frame.compression == Compression::none) {
        if (*encoded_size != *uncompressed_size) {
            return std::unexpected(make_error(
                ErrorCode::protocol_error,
                "compression=none requires encoded_size == uncompressed_size"));
        }
    } else {
        if (*encoded_size == 0 && *uncompressed_size != 0) {
            return std::unexpected(make_error(ErrorCode::protocol_error,
                                              "compressed ObjectFrame has empty encoded_data"));
        }
        // Rejeita antes de alocar o buffer descomprimido.
        const auto max_allowed =
            static_cast<std::uint64_t>(*encoded_size) * static_cast<std::uint64_t>(max_expansion_ratio);
        if (static_cast<std::uint64_t>(*uncompressed_size) > max_allowed) {
            return std::unexpected(make_error(
                ErrorCode::protocol_error,
                "ObjectFrame expansion ratio exceeds negotiated limit"));
        }
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

    std::vector<std::byte> uncompressed_storage;
    std::span<const std::byte> data_view = *encoded;
    if (frame.compression == Compression::rle) {
        auto inflated = decompress_rle(*encoded, *uncompressed_size);
        if (!inflated) {
            return std::unexpected(inflated.error());
        }
        if (inflated->size() != *uncompressed_size) {
            return std::unexpected(make_error(
                ErrorCode::protocol_error, "RLE decompress size mismatch"));
        }
        uncompressed_storage = std::move(*inflated);
        data_view = uncompressed_storage;
    }

    frame.records.reserve(slots.size());
    for (const auto& [offset, length] : slots) {
        const auto slice = data_view.subspan(offset, length);
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
            } else if constexpr (std::is_same_v<T, OpCall>) {
                return encode_op_call(writer, body);
            } else if constexpr (std::is_same_v<T, OpResult>) {
                return encode_op_result(writer, body);
            }
        },
        message);
}

Result<Message> decode_payload(MessageType type, storage::BinaryReader& reader,
                               std::uint32_t negotiated_max_frame,
                               std::uint16_t max_expansion_ratio) {
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
        auto body =
            decode_object_frame_payload(reader, negotiated_max_frame, max_expansion_ratio);
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
    case MessageType::op_call: {
        auto body = decode_op_call(reader);
        if (!body) {
            return std::unexpected(body.error());
        }
        return Message{std::move(*body)};
    }
    case MessageType::op_result: {
        auto body = decode_op_result(reader);
        if (!body) {
            return std::unexpected(body.error());
        }
        return Message{std::move(*body)};
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
            } else if constexpr (std::is_same_v<T, OpCall>) {
                return MessageType::op_call;
            } else if constexpr (std::is_same_v<T, OpResult>) {
                return MessageType::op_result;
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
    return decode_message(bytes, max_frame_bytes, default_max_expansion_ratio);
}

Result<Message> decode_message(std::span<const std::byte> bytes,
                               std::uint32_t negotiated_max_frame,
                               std::uint16_t max_expansion_ratio) {
    if (bytes.size() < 4) {
        return std::unexpected(
            make_error(ErrorCode::unexpected_end_of_input, "frame length truncated"));
    }

    storage::BinaryReader header{bytes.subspan(0, 4)};
    const auto length = header.read_u32();
    if (!length) {
        return std::unexpected(length.error());
    }
    if (*length > negotiated_max_frame || *length > max_frame_bytes) {
        return std::unexpected(
            make_error(ErrorCode::frame_too_large, "frame length exceeds negotiated max"));
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
    auto message = decode_payload(type, body, negotiated_max_frame, max_expansion_ratio);
    if (!message) {
        return message;
    }
    if (!body.at_end()) {
        return std::unexpected(
            make_error(ErrorCode::trailing_data, "bytes remain after message payload"));
    }
    return message;
}

Result<std::vector<std::byte>> compress_rle(std::span<const std::byte> input) {
    // Formato: blocos (tag u8, ...).
    //   tag=0: literal — len u8 (1..255), depois len bytes
    //   tag=1: repetição — count u16 (4..65535), byte u8
    std::vector<std::byte> out;
    out.reserve(input.size() + 8);
    std::size_t i = 0;
    while (i < input.size()) {
        std::size_t run = 1;
        while (i + run < input.size() && input[i + run] == input[i] && run < 65535) {
            ++run;
        }
        if (run >= 4) {
            out.push_back(std::byte{1});
            out.push_back(std::byte{static_cast<unsigned char>(run & 0xffu)});
            out.push_back(std::byte{static_cast<unsigned char>((run >> 8) & 0xffu)});
            out.push_back(input[i]);
            i += run;
            continue;
        }
        std::size_t lit = 0;
        while (i + lit < input.size() && lit < 255) {
            std::size_t peek = 1;
            while (i + lit + peek < input.size() && input[i + lit + peek] == input[i + lit] &&
                   peek < 65535) {
                ++peek;
            }
            if (peek >= 4) {
                break;
            }
            ++lit;
        }
        if (lit == 0) {
            // Força pelo menos 1 literal se um run curto ficou residual.
            lit = 1;
        }
        out.push_back(std::byte{0});
        out.push_back(std::byte{static_cast<unsigned char>(lit)});
        for (std::size_t j = 0; j < lit; ++j) {
            out.push_back(input[i + j]);
        }
        i += lit;
    }
    return out;
}

Result<std::vector<std::byte>> decompress_rle(std::span<const std::byte> input,
                                              std::uint32_t uncompressed_size) {
    std::vector<std::byte> out;
    out.reserve(uncompressed_size);
    std::size_t i = 0;
    while (i < input.size()) {
        if (out.size() > uncompressed_size) {
            return std::unexpected(
                make_error(ErrorCode::protocol_error, "RLE output exceeds uncompressed_size"));
        }
        const auto tag = std::to_integer<std::uint8_t>(input[i++]);
        if (tag == 0) {
            if (i >= input.size()) {
                return std::unexpected(
                    make_error(ErrorCode::unexpected_end_of_input, "RLE literal length truncated"));
            }
            const auto len = std::to_integer<std::uint8_t>(input[i++]);
            if (len == 0) {
                return std::unexpected(
                    make_error(ErrorCode::protocol_error, "RLE literal length is zero"));
            }
            if (i + len > input.size()) {
                return std::unexpected(
                    make_error(ErrorCode::unexpected_end_of_input, "RLE literal data truncated"));
            }
            if (out.size() + len > uncompressed_size) {
                return std::unexpected(
                    make_error(ErrorCode::protocol_error, "RLE literal overflows uncompressed_size"));
            }
            out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(i),
                       input.begin() + static_cast<std::ptrdiff_t>(i + len));
            i += len;
        } else if (tag == 1) {
            if (i + 3 > input.size()) {
                return std::unexpected(
                    make_error(ErrorCode::unexpected_end_of_input, "RLE repeat truncated"));
            }
            const auto count =
                static_cast<std::size_t>(std::to_integer<std::uint8_t>(input[i])) |
                (static_cast<std::size_t>(std::to_integer<std::uint8_t>(input[i + 1])) << 8);
            const auto value = input[i + 2];
            i += 3;
            if (count < 4) {
                return std::unexpected(
                    make_error(ErrorCode::protocol_error, "RLE repeat count must be >= 4"));
            }
            if (out.size() + count > uncompressed_size) {
                return std::unexpected(
                    make_error(ErrorCode::protocol_error, "RLE repeat overflows uncompressed_size"));
            }
            out.insert(out.end(), count, value);
        } else {
            return std::unexpected(make_error(ErrorCode::protocol_error, "RLE unknown block tag"));
        }
    }
    if (out.size() != uncompressed_size) {
        return std::unexpected(
            make_error(ErrorCode::protocol_error, "RLE decompress size mismatch"));
    }
    return out;
}

} // namespace modb::net
