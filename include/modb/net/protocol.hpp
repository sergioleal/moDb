#pragma once

// Codec do protocolo binário da Fase 8A: frames versionados sem rede.
// Layout e regras: PROTOCOLO_FASES.md §Fase 8 e ADR-010/ADR-011.
// Compressão nesta entrega: somente `none` (codecs comprimidos fecham na 8F).

#include "modb/error.hpp"
#include "modb/net/query_description.hpp"
#include "modb/object/ids.hpp"
#include "modb/storage/binary.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace modb::net {

inline constexpr std::uint16_t protocol_version = 1;
// length cobre type+payload; frames maiores → frame_too_large.
inline constexpr std::uint32_t max_frame_bytes = 16u * 1024u * 1024u;
// Limite defensivo para strings do protocolo (nome do banco, mensagem de erro).
inline constexpr std::uint32_t max_string_bytes = 64u * 1024u;

enum class MessageType : std::uint8_t {
    hello = 1,
    hello_ok = 2,
    query = 3,
    stream_begin = 4,
    object_frame = 5,
    stream_end = 6,
    stream_error = 7,
    cancel = 8,
    // 9 OpCall / 10 OpResult — reservados para a Fase 9.
};

enum class Compression : std::uint8_t {
    none = 0,
};

struct Hello {
    std::uint16_t version{protocol_version};
    std::string database_name{};
    std::vector<Compression> accepted_codecs{Compression::none};

    friend bool operator==(const Hello&, const Hello&) = default;
};

struct HelloOk {
    std::uint16_t version{protocol_version};
    object::BaselineId baseline{};
    Compression selected_codec{Compression::none};
    std::uint32_t max_frame_bytes{modb::net::max_frame_bytes};

    friend bool operator==(const HelloOk&, const HelloOk&) = default;
};

struct Query {
    std::uint32_t query_id{0};
    QueryDescription description{};

    friend bool operator==(const Query&, const Query&) = default;
};

struct StreamBegin {
    std::uint32_t query_id{0};

    friend bool operator==(const StreamBegin&, const StreamBegin&) = default;
};

// Envelope lógico: identidade + tipo + payload do codec genérico (ADR-003).
// Nunca carrega PageId / SlotId / RecordId.
struct ObjectEnvelope {
    object::ObjectId object_id{};
    object::TypeDefinitionId type_definition_id{};
    std::vector<std::byte> payload{};

    friend bool operator==(const ObjectEnvelope&, const ObjectEnvelope&) = default;
};

struct ObjectFrame {
    std::uint32_t query_id{0};
    Compression compression{Compression::none};
    std::vector<ObjectEnvelope> records{};

    friend bool operator==(const ObjectFrame&, const ObjectFrame&) = default;
};

struct StreamEnd {
    std::uint32_t query_id{0};
    std::uint64_t total{0};

    friend bool operator==(const StreamEnd&, const StreamEnd&) = default;
};

struct StreamError {
    std::uint32_t query_id{0};
    ErrorCode code{ErrorCode::protocol_error};
    std::string message{};

    friend bool operator==(const StreamError&, const StreamError&) = default;
};

struct Cancel {
    std::uint32_t query_id{0};

    friend bool operator==(const Cancel&, const Cancel&) = default;
};

using Message = std::variant<Hello, HelloOk, Query, StreamBegin, ObjectFrame, StreamEnd,
                             StreamError, Cancel>;

[[nodiscard]] MessageType message_type(const Message& message) noexcept;

// Codifica uma mensagem completa: | length u32 | type u8 | payload |.
[[nodiscard]] Result<std::vector<std::byte>> encode_message(const Message& message);

// Decodifica um frame completo a partir de um buffer. Valida length antes de
// alocar; frames > max_frame_bytes → frame_too_large; entradas hostis →
// protocol_error (nunca alocação gigante).
[[nodiscard]] Result<Message> decode_message(std::span<const std::byte> bytes);

// Codifica/decodifica só o envelope (também usado dentro de ObjectFrame).
[[nodiscard]] Result<void> encode_object_envelope(storage::BinaryWriter& writer,
                                                  const ObjectEnvelope& envelope);
[[nodiscard]] Result<ObjectEnvelope> decode_object_envelope(storage::BinaryReader& reader);

} // namespace modb::net
