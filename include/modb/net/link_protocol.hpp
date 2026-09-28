#pragma once

// Link local proxy ↔ engine (ADR-028, PLANO_PROXY X3).
//
// Um link carrega muitas sessões de cliente. Cada frame é um frame do
// protocolo do cliente precedido do número da sessão:
//
//   | session u32 | length u32 | type u8 | payload |
//
// Depois do `session` vem um frame do cliente exatamente como no fio de hoje
// (`encode_message`/`decode_message`), então as mensagens de sessão passam
// pelo link sem outra codificação. As mensagens de controle do link usam tipos
// a partir de 0x80, que o protocolo do cliente nunca usa:
//
//   session = 0  → LinkHello / LinkHelloOk (abertura do link)
//   session ≠ 0  → SessionOpen / SessionOpenOk / SessionClose / StreamCredit,
//                  e as mensagens do protocolo do cliente daquela sessão
//
// O link não usa compressão: é local. A compressão fica entre proxy e cliente.

#include "modb/error.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/net/protocol.hpp"
#include "modb/object/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace modb::net {

inline constexpr std::uint16_t link_version = 1;
// Sessão do canal de controle do link.
inline constexpr std::uint32_t link_control_session = 0;

enum class LinkMessageType : std::uint8_t {
    link_hello = 0x81,
    link_hello_ok = 0x82,
    session_open = 0x83,
    session_open_ok = 0x84,
    session_close = 0x85,
    stream_credit = 0x86,
};

// Proxy → engine, primeira mensagem do link.
struct LinkHello {
    std::uint16_t version{link_version};
    // Nome do proxy (logs do engine).
    std::string proxy_name{};
    // Segredo compartilhado (vazio = nenhum). O engine compara em tempo constante.
    std::string secret{};

    friend bool operator==(const LinkHello&, const LinkHello&) = default;
};

// Engine → proxy. `ok = false` fecha o link (versão ou segredo recusados).
struct LinkHelloOk {
    std::uint16_t version{link_version};
    bool ok{true};
    ErrorCode code{ErrorCode::invalid_argument};
    std::string message{};
    // O que o proxy repassa no HelloOk de cada cliente.
    std::uint16_t protocol_major{net::protocol_major};
    std::uint16_t protocol_minor{net::protocol_minor};
    object::BaselineId baseline{};
    std::string database_name{};
    std::uint16_t max_concurrent_streams{default_max_concurrent_streams};

    friend bool operator==(const LinkHelloOk&, const LinkHelloOk&) = default;
};

// Proxy → engine: um cliente entrou. O proxy já o autenticou; o engine só
// recebe quem é.
struct SessionOpen {
    // Minor do protocolo negociado com o cliente.
    std::uint16_t client_minor{net::protocol_minor};
    std::string principal{};
    std::vector<std::string> roles{};
    std::vector<std::pair<std::string, std::string>> attributes{};

    friend bool operator==(const SessionOpen&, const SessionOpen&) = default;
};

struct SessionOpenOk {
    bool ok{true};
    ErrorCode code{ErrorCode::invalid_argument};
    std::string message{};

    friend bool operator==(const SessionOpenOk&, const SessionOpenOk&) = default;
};

// Qualquer lado: a sessão acabou (cliente saiu, ou o engine a encerrou por
// erro de protocolo). Quem recebe descarta o estado dela.
struct SessionClose {
    ErrorCode code{ErrorCode::connection_closed};
    std::string message{};

    friend bool operator==(const SessionClose&, const SessionClose&) = default;
};

// Proxy → engine: o stream `query_id` pode mandar mais `frames` ObjectFrames.
struct StreamCredit {
    std::uint32_t query_id{0};
    std::uint32_t frames{0};

    friend bool operator==(const StreamCredit&, const StreamCredit&) = default;
};

using LinkControl = std::variant<LinkHello, LinkHelloOk, SessionOpen, SessionOpenOk, SessionClose, StreamCredit>;

// Um frame do link: controle do link ou mensagem do protocolo do cliente.
struct LinkFrame {
    std::uint32_t session{link_control_session};
    std::variant<Message, LinkControl> body{};

    friend bool operator==(const LinkFrame&, const LinkFrame&) = default;
};

// Limites do link: frames do protocolo do cliente (16 MiB) e strings.
inline constexpr std::size_t max_link_roles = 256;
inline constexpr std::size_t max_link_attributes = 256;

[[nodiscard]] Result<std::vector<std::byte>> encode_link_frame(const LinkFrame& frame);
// `bytes` = um frame completo (session + length + type + payload). Entradas
// hostis → protocol_error; nunca aloca pelo tamanho declarado sem validar.
[[nodiscard]] Result<LinkFrame> decode_link_frame(std::span<const std::byte> bytes);

[[nodiscard]] Result<void> send_link_frame(NativeSocket& socket, const LinkFrame& frame);
[[nodiscard]] Result<LinkFrame> recv_link_frame(NativeSocket& socket);

} // namespace modb::net
