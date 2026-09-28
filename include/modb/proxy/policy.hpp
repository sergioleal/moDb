#pragma once

// Política de um proxy (ADR-028, PLANO_PROXY X5.3).
//
// O proxy decodifica cada mensagem do cliente e a passa pela política antes de
// mandá-la ao engine: a política autentica o cliente, autoriza (ou recusa, ou
// reescreve no lugar) cada pedido, vê cada resposta antes de ela voltar ao
// cliente e recebe um registro de auditoria por pedido concluído. Cada
// implementação de proxy é um transporte mais uma política.
//
// Os métodos são chamados de várias threads (uma por cliente) ao mesmo tempo:
// a política cuida da própria sincronização.

#include "modb/error.hpp"
#include "modb/net/protocol.hpp"
#include "modb/ops/caller.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modb::proxy {

// O que o proxy sabe do cliente antes de ele se identificar.
struct ClientInfo {
    // "a.b.c.d:porta".
    std::string address{};
    bool tls{false};
};

// O que o cliente mandou para se identificar (X6). `mechanism` vazio = não
// mandou nada.
struct Credentials {
    std::string mechanism{};
    std::vector<std::byte> payload{};
};

struct Decision {
    bool allowed{true};
    ErrorCode code{ErrorCode::permission_denied};
    std::string message{};

    [[nodiscard]] static Decision allow() { return Decision{}; }
    [[nodiscard]] static Decision deny(std::string message, ErrorCode code = ErrorCode::permission_denied) {
        return Decision{.allowed = false, .code = code, .message = std::move(message)};
    }
};

// O engine atrás do proxy, a cada abertura do link (inclusive reconexões).
struct EngineInfo {
    std::string database_name{};
    std::uint64_t baseline{0};
    struct Operation {
        std::string id;
        bool read_only{false};
    };
    // Em ordem de id.
    std::vector<Operation> operations{};

    // Modo da operação; vazio = o engine não a tem.
    [[nodiscard]] std::optional<bool> read_only(std::string_view id) const;
};

// Um pedido concluído (ou recusado pela política).
struct AuditRecord {
    const ops::Caller* caller{nullptr};
    std::string_view client{};     // ClientInfo::address
    std::string_view kind{};       // "call", "query", "facade_list", "facade_open"
    std::string_view target{};     // id da operação, tipo consultado, facade
    bool ok{true};
    ErrorCode code{ErrorCode::invalid_argument};
    std::string_view message{};
    bool denied{false};            // recusado pela política, sem chegar ao engine
    std::chrono::nanoseconds duration{};
    std::uint64_t objects{0};      // objetos entregues (consultas)
};

class Policy {
public:
    Policy() = default;
    Policy(const Policy&) = delete;
    Policy& operator=(const Policy&) = delete;
    virtual ~Policy() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // Mecanismos de autenticação aceitos (anunciados ao cliente, X6). Vazio =
    // a política não exige autenticação: `authenticate` recebe credenciais vazias.
    [[nodiscard]] virtual std::vector<std::string> mechanisms() const { return {}; }

    // Quem é o cliente. Erro (em geral `unauthenticated`) = o cliente não entra.
    // O padrão aceita todos como anônimos, com o endereço como atributo.
    [[nodiscard]] virtual Result<ops::Caller> authenticate(const ClientInfo& client, const Credentials& credentials);

    // Pode fazer isto? A política pode reescrever `request` no lugar (menos o
    // id do pedido, que o proxy usa para casar a resposta). `Cancel` não passa
    // por aqui: cancelar o próprio stream é sempre permitido.
    [[nodiscard]] virtual Decision authorize(const ops::Caller& caller, net::Message& request);

    // Cada resposta do engine antes de ir ao cliente; pode reescrevê-la.
    virtual void on_response(const ops::Caller& caller, net::Message& response);

    virtual void audit(const AuditRecord& record);

    // O link com o engine abriu (ou reabriu): o catálogo pode ter mudado.
    // Chamado da thread do link, junto com as outras chamadas dos clientes.
    virtual void on_engine(const EngineInfo& engine);
};

// Deixa tudo passar, sem autenticação.
class PassThroughPolicy final : public Policy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "passthrough"; }
};

} // namespace modb::proxy
