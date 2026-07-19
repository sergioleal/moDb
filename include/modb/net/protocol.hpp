#pragma once

// Codec do protocolo binário da Fase 8: frames versionados sem rede.
// Layout e regras: PROTOCOLO_FASES.md §Fase 8 e ADR-010/ADR-011.
// Compressão: `none` obrigatório; `rle` negociável (Fase 8F; fallback para none).

#include "modb/error.hpp"
#include "modb/net/query_description.hpp"
#include "modb/object/ids.hpp"
#include "modb/ops/facade_descriptor.hpp"
#include "modb/storage/binary.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace modb::net {

// Major do protocolo no fio (`Hello.version` / `HelloOk.version`).
inline constexpr std::uint16_t protocol_major = 1;
// Minor aditivo (Fase 10E); extensões desconhecidas no Hello/HelloOk são ignoráveis.
inline constexpr std::uint16_t protocol_minor = 0;
// Alias legado (= major).
inline constexpr std::uint16_t protocol_version = protocol_major;
// length cobre type+payload; frames maiores → frame_too_large.
inline constexpr std::uint32_t max_frame_bytes = 16u * 1024u * 1024u;
// Limite defensivo para strings do protocolo (nome do banco, mensagem de erro).
inline constexpr std::uint32_t max_string_bytes = 64u * 1024u;

// Políticas de recurso (Fase 8F) — anunciadas no HelloOk.
inline constexpr std::uint16_t default_max_concurrent_streams = 4;
inline constexpr std::uint16_t default_max_expansion_ratio = 8; // uncompressed ≤ encoded × ratio
inline constexpr std::uint32_t default_idle_timeout_ms = 30'000;
// Só tenta comprimir frames com área de dados ≥ este limiar.
inline constexpr std::uint32_t compression_min_bytes = 64;

enum class MessageType : std::uint8_t {
    hello = 1,
    hello_ok = 2,
    query = 3,
    stream_begin = 4,
    object_frame = 5,
    stream_end = 6,
    stream_error = 7,
    cancel = 8,
    op_call = 9,
    op_result = 10,
    facade_list = 11,
    facade_list_ok = 12,
    facade_open = 13,
    facade_open_ok = 14,
};

enum class Compression : std::uint8_t {
    none = 0,
    // RLE embutido (sem dependência externa); LZ4/Zstd ficam para benchmark futuro.
    rle = 1,
};

[[nodiscard]] constexpr bool is_known_compression(Compression codec) noexcept {
    return codec == Compression::none || codec == Compression::rle;
}

struct Hello {
    // Major do protocolo (campo histórico `version` no fio).
    std::uint16_t version{protocol_major};
    // Minor aditivo gravado após os codecs (ausente em peers antigos → 0).
    std::uint16_t minor{protocol_minor};
    std::string database_name{};
    std::vector<Compression> accepted_codecs{Compression::none};

    friend bool operator==(const Hello&, const Hello&) = default;
};

struct HelloOk {
    std::uint16_t version{protocol_major};
    std::uint16_t minor{protocol_minor};
    object::BaselineId baseline{};
    Compression selected_codec{Compression::none};
    std::uint32_t max_frame_bytes{modb::net::max_frame_bytes};
    std::uint16_t max_concurrent_streams{default_max_concurrent_streams};
    std::uint16_t max_expansion_ratio{default_max_expansion_ratio};
    std::uint32_t idle_timeout_ms{default_idle_timeout_ms};

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
    // Codec preferido na codificação; o encoder pode fazer fallback para none.
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

struct OpCall {
    std::uint32_t call_id{0};
    std::string operation_id{};
    std::vector<std::byte> args{};

    friend bool operator==(const OpCall&, const OpCall&) = default;
};

struct OpResult {
    std::uint32_t call_id{0};
    bool ok{true};
    ErrorCode code{ErrorCode::invalid_argument};
    std::string message{};
    std::vector<std::byte> payload{};

    friend bool operator==(const OpResult&, const OpResult&) = default;
};

// Fase 11C: descoberta e negociação de facades.
struct FacadeList {
    std::uint32_t request_id{0};

    friend bool operator==(const FacadeList&, const FacadeList&) = default;
};

struct FacadeListOk {
    std::uint32_t request_id{0};
    std::vector<ops::FacadeDescriptor> facades{};

    friend bool operator==(const FacadeListOk&, const FacadeListOk&) = default;
};

struct FacadeOpen {
    std::uint32_t request_id{0};
    std::string facade_id{};
    std::uint32_t facade_version{1};

    friend bool operator==(const FacadeOpen&, const FacadeOpen&) = default;
};

struct FacadeOpenOk {
    std::uint32_t request_id{0};
    bool ok{true};
    ErrorCode code{ErrorCode::invalid_argument};
    std::string message{};
    std::string facade_id{};
    std::uint32_t facade_version{0};

    friend bool operator==(const FacadeOpenOk&, const FacadeOpenOk&) = default;
};

using Message = std::variant<Hello, HelloOk, Query, StreamBegin, ObjectFrame, StreamEnd,
                             StreamError, Cancel, OpCall, OpResult, FacadeList, FacadeListOk,
                             FacadeOpen, FacadeOpenOk>;

[[nodiscard]] MessageType message_type(const Message& message) noexcept;

// Codifica uma mensagem completa: | length u32 | type u8 | payload |.
[[nodiscard]] Result<std::vector<std::byte>> encode_message(const Message& message);

// Decodifica um frame completo a partir de um buffer. Valida length antes de
// alocar; frames > max_frame_bytes → frame_too_large; entradas hostis →
// protocol_error (nunca alocação gigante).
[[nodiscard]] Result<Message> decode_message(std::span<const std::byte> bytes);

// Variante com limites negociados (expansão / frame) da conexão.
[[nodiscard]] Result<Message> decode_message(std::span<const std::byte> bytes,
                                             std::uint32_t negotiated_max_frame,
                                             std::uint16_t max_expansion_ratio);

// Codifica/decodifica só o envelope (também usado dentro de ObjectFrame).
[[nodiscard]] Result<void> encode_object_envelope(storage::BinaryWriter& writer,
                                                  const ObjectEnvelope& envelope);
[[nodiscard]] Result<ObjectEnvelope> decode_object_envelope(storage::BinaryReader& reader);

// Compressão RLE (área de dados do ObjectFrame). Expostas para testes unitários.
[[nodiscard]] Result<std::vector<std::byte>> compress_rle(std::span<const std::byte> input);
[[nodiscard]] Result<std::vector<std::byte>> decompress_rle(std::span<const std::byte> input,
                                                            std::uint32_t uncompressed_size);

} // namespace modb::net
