#include "modb/net/link_protocol.hpp"

#include "modb/storage/binary.hpp"
#include "modb/storage/endian.hpp"

#include <array>
#include <cstring>
#include <string_view>

namespace modb::net {
namespace {

constexpr std::size_t k_header_bytes = 8;  // session u32 + length u32

Error make_protocol(std::string message) {
    return Error{ErrorCode::protocol_error, std::move(message)};
}

Result<void> write_string(storage::BinaryWriter& writer, std::string_view text) {
    if (text.size() > max_string_bytes) {
        return std::unexpected(Error{ErrorCode::value_too_large, "link string exceeds max_string_bytes"});
    }
    writer.write_u32(static_cast<std::uint32_t>(text.size()));
    writer.write_bytes(std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()), text.size()});
    return {};
}

Result<std::string> read_string(storage::BinaryReader& reader) {
    const auto length = reader.read_u32();
    if (!length) {
        return std::unexpected(length.error());
    }
    if (*length > max_string_bytes) {
        return std::unexpected(make_protocol("link string length exceeds limit"));
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

Result<bool> read_bool(storage::BinaryReader& reader, std::string_view what) {
    const auto raw = reader.read_u8();
    if (!raw) {
        return std::unexpected(raw.error());
    }
    if (*raw > 1) {
        return std::unexpected(make_protocol(std::string{what} + " must be 0 or 1"));
    }
    return *raw == 1;
}

Result<ErrorCode> read_code(storage::BinaryReader& reader) {
    const auto raw = reader.read_u16();
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return static_cast<ErrorCode>(*raw);
}

// Lê um campo e devolve o erro do jeito curto: `READ(x, read_u32())`.
#define MODB_LINK_READ(target, expr)                    \
    do {                                                \
        auto modb_link_value = (expr);                  \
        if (!modb_link_value) {                         \
            return std::unexpected(modb_link_value.error()); \
        }                                               \
        target = std::move(*modb_link_value);           \
    } while (false)

#define MODB_LINK_TRY(expr)                             \
    do {                                                \
        if (auto modb_link_status = (expr); !modb_link_status) { \
            return std::unexpected(modb_link_status.error()); \
        }                                               \
    } while (false)

Result<void> encode_control(storage::BinaryWriter& writer, const LinkControl& control) {
    return std::visit(
        [&](const auto& message) -> Result<void> {
            using T = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<T, LinkHello>) {
                writer.write_u8(static_cast<std::uint8_t>(LinkMessageType::link_hello));
                writer.write_u16(message.version);
                MODB_LINK_TRY(write_string(writer, message.proxy_name));
                MODB_LINK_TRY(write_string(writer, message.secret));
            } else if constexpr (std::is_same_v<T, LinkHelloOk>) {
                writer.write_u8(static_cast<std::uint8_t>(LinkMessageType::link_hello_ok));
                writer.write_u16(message.version);
                writer.write_u8(message.ok ? 1 : 0);
                writer.write_u16(static_cast<std::uint16_t>(message.code));
                MODB_LINK_TRY(write_string(writer, message.message));
                writer.write_u16(message.protocol_major);
                writer.write_u16(message.protocol_minor);
                writer.write_u64(message.baseline.value);
                MODB_LINK_TRY(write_string(writer, message.database_name));
                writer.write_u16(message.max_concurrent_streams);
            } else if constexpr (std::is_same_v<T, SessionOpen>) {
                if (message.roles.size() > max_link_roles || message.attributes.size() > max_link_attributes) {
                    return std::unexpected(Error{ErrorCode::value_too_large, "too many roles or attributes"});
                }
                writer.write_u8(static_cast<std::uint8_t>(LinkMessageType::session_open));
                writer.write_u16(message.client_minor);
                MODB_LINK_TRY(write_string(writer, message.principal));
                writer.write_u16(static_cast<std::uint16_t>(message.roles.size()));
                for (const auto& role : message.roles) {
                    MODB_LINK_TRY(write_string(writer, role));
                }
                writer.write_u16(static_cast<std::uint16_t>(message.attributes.size()));
                for (const auto& [key, value] : message.attributes) {
                    MODB_LINK_TRY(write_string(writer, key));
                    MODB_LINK_TRY(write_string(writer, value));
                }
            } else if constexpr (std::is_same_v<T, SessionOpenOk>) {
                writer.write_u8(static_cast<std::uint8_t>(LinkMessageType::session_open_ok));
                writer.write_u8(message.ok ? 1 : 0);
                writer.write_u16(static_cast<std::uint16_t>(message.code));
                MODB_LINK_TRY(write_string(writer, message.message));
            } else if constexpr (std::is_same_v<T, SessionClose>) {
                writer.write_u8(static_cast<std::uint8_t>(LinkMessageType::session_close));
                writer.write_u16(static_cast<std::uint16_t>(message.code));
                MODB_LINK_TRY(write_string(writer, message.message));
            } else {
                static_assert(std::is_same_v<T, StreamCredit>);
                writer.write_u8(static_cast<std::uint8_t>(LinkMessageType::stream_credit));
                writer.write_u32(message.query_id);
                writer.write_u32(message.frames);
            }
            return {};
        },
        control);
}

Result<LinkControl> decode_control(LinkMessageType type, storage::BinaryReader& reader) {
    switch (type) {
    case LinkMessageType::link_hello: {
        LinkHello message;
        MODB_LINK_READ(message.version, reader.read_u16());
        MODB_LINK_READ(message.proxy_name, read_string(reader));
        MODB_LINK_READ(message.secret, read_string(reader));
        return message;
    }
    case LinkMessageType::link_hello_ok: {
        LinkHelloOk message;
        MODB_LINK_READ(message.version, reader.read_u16());
        MODB_LINK_READ(message.ok, read_bool(reader, "LinkHelloOk ok"));
        MODB_LINK_READ(message.code, read_code(reader));
        MODB_LINK_READ(message.message, read_string(reader));
        MODB_LINK_READ(message.protocol_major, reader.read_u16());
        MODB_LINK_READ(message.protocol_minor, reader.read_u16());
        MODB_LINK_READ(message.baseline.value, reader.read_u64());
        MODB_LINK_READ(message.database_name, read_string(reader));
        MODB_LINK_READ(message.max_concurrent_streams, reader.read_u16());
        return message;
    }
    case LinkMessageType::session_open: {
        SessionOpen message;
        MODB_LINK_READ(message.client_minor, reader.read_u16());
        MODB_LINK_READ(message.principal, read_string(reader));
        std::uint16_t roles = 0;
        MODB_LINK_READ(roles, reader.read_u16());
        if (roles > max_link_roles) {
            return std::unexpected(make_protocol("SessionOpen has too many roles"));
        }
        for (std::uint16_t i = 0; i < roles; ++i) {
            std::string role;
            MODB_LINK_READ(role, read_string(reader));
            message.roles.push_back(std::move(role));
        }
        std::uint16_t attributes = 0;
        MODB_LINK_READ(attributes, reader.read_u16());
        if (attributes > max_link_attributes) {
            return std::unexpected(make_protocol("SessionOpen has too many attributes"));
        }
        for (std::uint16_t i = 0; i < attributes; ++i) {
            std::string key;
            std::string value;
            MODB_LINK_READ(key, read_string(reader));
            MODB_LINK_READ(value, read_string(reader));
            message.attributes.emplace_back(std::move(key), std::move(value));
        }
        return message;
    }
    case LinkMessageType::session_open_ok: {
        SessionOpenOk message;
        MODB_LINK_READ(message.ok, read_bool(reader, "SessionOpenOk ok"));
        MODB_LINK_READ(message.code, read_code(reader));
        MODB_LINK_READ(message.message, read_string(reader));
        return message;
    }
    case LinkMessageType::session_close: {
        SessionClose message;
        MODB_LINK_READ(message.code, read_code(reader));
        MODB_LINK_READ(message.message, read_string(reader));
        return message;
    }
    case LinkMessageType::stream_credit: {
        StreamCredit message;
        MODB_LINK_READ(message.query_id, reader.read_u32());
        MODB_LINK_READ(message.frames, reader.read_u32());
        return message;
    }
    }
    return std::unexpected(make_protocol("unknown link message type"));
}

#undef MODB_LINK_READ
#undef MODB_LINK_TRY

[[nodiscard]] bool is_link_scoped(const LinkControl& control) noexcept {
    return std::holds_alternative<LinkHello>(control) || std::holds_alternative<LinkHelloOk>(control);
}

Result<void> check_session(const LinkFrame& frame) {
    const bool control_session = frame.session == link_control_session;
    if (const auto* control = std::get_if<LinkControl>(&frame.body)) {
        if (is_link_scoped(*control) != control_session) {
            return std::unexpected(make_protocol(control_session ? "session message on the link control session"
                                                                 : "link message on a client session"));
        }
        return {};
    }
    if (control_session) {
        return std::unexpected(make_protocol("client message on the link control session"));
    }
    return {};
}

} // namespace

Result<std::vector<std::byte>> encode_link_frame(const LinkFrame& frame) {
    if (auto status = check_session(frame); !status) {
        return std::unexpected(status.error());
    }
    std::vector<std::byte> out(4);
    storage::store_le<std::uint32_t>(std::span<std::byte>{out.data(), 4}, frame.session);
    if (const auto* message = std::get_if<Message>(&frame.body)) {
        auto encoded = encode_message(*message);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        out.insert(out.end(), encoded->begin(), encoded->end());
        return out;
    }
    storage::BinaryWriter body;
    if (auto status = encode_control(body, std::get<LinkControl>(frame.body)); !status) {
        return std::unexpected(status.error());
    }
    const auto bytes = body.bytes();
    if (bytes.size() > max_frame_bytes) {
        return std::unexpected(Error{ErrorCode::frame_too_large, "link frame exceeds max_frame_bytes"});
    }
    out.resize(k_header_bytes);
    storage::store_le<std::uint32_t>(std::span<std::byte>{out.data() + 4, 4}, static_cast<std::uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

Result<LinkFrame> decode_link_frame(std::span<const std::byte> bytes) {
    if (bytes.size() < k_header_bytes + 1) {
        return std::unexpected(Error{ErrorCode::unexpected_end_of_input, "link frame truncated"});
    }
    LinkFrame frame;
    frame.session = storage::load_le<std::uint32_t>(bytes.first(4));
    const auto length = storage::load_le<std::uint32_t>(bytes.subspan(4, 4));
    if (length > max_frame_bytes) {
        return std::unexpected(Error{ErrorCode::frame_too_large, "link frame length exceeds max"});
    }
    const auto type = std::to_integer<std::uint8_t>(bytes[k_header_bytes]);
    if (type >= 0x80) {
        if (bytes.size() != k_header_bytes + length) {
            return std::unexpected(Error{bytes.size() < k_header_bytes + length ? ErrorCode::unexpected_end_of_input
                                                                                : ErrorCode::trailing_data,
                                         "link frame length does not match its bytes"});
        }
        storage::BinaryReader reader{bytes.subspan(k_header_bytes + 1, length - 1)};
        auto control = decode_control(static_cast<LinkMessageType>(type), reader);
        if (!control) {
            return std::unexpected(control.error());
        }
        if (!reader.at_end()) {
            return std::unexpected(Error{ErrorCode::trailing_data, "bytes remain after link message"});
        }
        frame.body = std::move(*control);
    } else {
        auto message = decode_message(bytes.subspan(4));
        if (!message) {
            return std::unexpected(message.error());
        }
        frame.body = std::move(*message);
    }
    if (auto status = check_session(frame); !status) {
        return std::unexpected(status.error());
    }
    return frame;
}

Result<void> send_link_frame(NativeSocket& socket, const LinkFrame& frame) {
    auto encoded = encode_link_frame(frame);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return socket.send_all(*encoded);
}

Result<void> send_link_message(NativeSocket& socket, std::uint32_t session, const Message& message) {
    if (session == link_control_session) {
        return std::unexpected(make_protocol("client message on the link control session"));
    }
    auto encoded = encode_message(message);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    std::array<std::byte, 4> prefix{};
    storage::store_le<std::uint32_t>(std::span<std::byte>{prefix}, session);
    // Um send só: frames de sessões diferentes se intercalam por mensagem inteira.
    std::vector<std::byte> out;
    out.reserve(prefix.size() + encoded->size());
    out.insert(out.end(), prefix.begin(), prefix.end());
    out.insert(out.end(), encoded->begin(), encoded->end());
    return socket.send_all(out);
}

Result<LinkFrame> recv_link_frame(NativeSocket& socket) {
    std::array<std::byte, k_header_bytes> header{};
    if (auto status = socket.recv_exact(header); !status) {
        return std::unexpected(status.error());
    }
    const auto length = storage::load_le<std::uint32_t>(std::span<const std::byte>{header}.subspan(4, 4));
    if (length == 0) {
        return std::unexpected(make_protocol("link frame length is zero"));
    }
    if (length > max_frame_bytes) {
        return std::unexpected(Error{ErrorCode::frame_too_large, "link frame length exceeds max"});
    }
    std::vector<std::byte> frame(k_header_bytes + length);
    std::copy(header.begin(), header.end(), frame.begin());
    if (auto status = socket.recv_exact(std::span<std::byte>{frame.data() + k_header_bytes, length}); !status) {
        return std::unexpected(status.error());
    }
    return decode_link_frame(frame);
}

} // namespace modb::net
