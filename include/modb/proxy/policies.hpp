#pragma once

// Políticas de referência do proxy (ADR-028, PLANO_PROXY X7) e a composição
// delas. Um proxy combina várias numa `PolicyChain`, nesta ordem:
//
//   autenticação (TokenPolicy) → allowlist → read_only → limites → auditoria
//
// Os limites ficam por último entre as que recusam: contam só o que de fato
// vai ao engine.

#include "modb/error.hpp"
#include "modb/proxy/policy.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace modb::proxy {

// Várias políticas como uma: a primeira com mecanismos autentica; cada pedido
// passa por todas, em ordem, e a primeira recusa vale; respostas, auditoria e
// o catálogo do engine vão a todas.
class PolicyChain final : public Policy {
public:
    explicit PolicyChain(std::vector<std::shared_ptr<Policy>> links);

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] std::vector<std::string> mechanisms() const override;
    [[nodiscard]] Result<ops::Caller> authenticate(const ClientInfo& client, const Credentials& credentials) override;
    [[nodiscard]] Decision authorize(const ops::Caller& caller, net::Message& request) override;
    void on_response(const ops::Caller& caller, net::Message& response) override;
    void audit(const AuditRecord& record) override;
    void on_engine(const EngineInfo& engine) override;

private:
    std::vector<std::shared_ptr<Policy>> links_;
    std::string name_;
};

// Só leitura: recusa as procs de escrita (e as que o engine não conhece),
// pelo catálogo que o engine manda na abertura do link. Consultas e facades
// passam (os métodos de uma facade são procs, e passam por aqui).
class ReadOnlyPolicy final : public Policy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "read_only"; }
    [[nodiscard]] Decision authorize(const ops::Caller& caller, net::Message& request) override;
    void on_engine(const EngineInfo& engine) override;

private:
    mutable std::shared_mutex mu_;
    EngineInfo engine_;
};

// Lista do que cada um pode fazer; o que nenhuma regra permite é recusado.
// Arquivo, uma regra por linha (`#` comenta):
//
//   <quem>  <tipo>  <alvo>
//
//   quem:  uma role, `user:<principal>`, ou `*` (qualquer um, até anônimo)
//   tipo:  call | query | facade | *
//   alvo:  id exato, prefixo terminado em `*`, ou `*`. Numa consulta, o alvo
//          é o id numérico do tipo; numa facade, o id da facade.
class AllowlistPolicy final : public Policy {
public:
    struct Rule {
        std::string who;
        std::string kind;
        std::string target;
    };

    explicit AllowlistPolicy(std::vector<Rule> rules) : rules_{std::move(rules)} {}
    [[nodiscard]] static Result<std::shared_ptr<AllowlistPolicy>> parse(std::string_view text);
    [[nodiscard]] static Result<std::shared_ptr<AllowlistPolicy>> load(const std::filesystem::path& file);

    [[nodiscard]] std::string_view name() const noexcept override { return "allowlist"; }
    [[nodiscard]] Decision authorize(const ops::Caller& caller, net::Message& request) override;

    [[nodiscard]] const std::vector<Rule>& rules() const noexcept { return rules_; }

private:
    std::vector<Rule> rules_;
};

// Uma linha por pedido concluído ou recusado, no formato do log de chamadas
// do servidor:
//
//   audit <kind> <target> ok|error <code> <ms>ms [denied] [objects N] by <principal|-> from <address>
class AuditLogPolicy final : public Policy {
public:
    using Sink = std::function<void(std::string_view line)>;

    explicit AuditLogPolicy(Sink sink) : sink_{std::move(sink)} {}
    // Escreve num stream (arquivo aberto ou std::cerr), uma linha por vez.
    [[nodiscard]] static std::shared_ptr<AuditLogPolicy> to_stream(std::ostream& out);

    [[nodiscard]] std::string_view name() const noexcept override { return "audit"; }
    void audit(const AuditRecord& record) override;

    [[nodiscard]] static std::string format(const AuditRecord& record);

private:
    Sink sink_;
};

// Limites por principal (anônimos: por endereço): chamadas por segundo
// (balde com rajada igual ao limite) e streams abertos ao mesmo tempo.
// 0 = sem limite.
class RateLimitPolicy final : public Policy {
public:
    using Clock = std::chrono::steady_clock;

    RateLimitPolicy(std::uint32_t calls_per_second, std::uint32_t streams_per_principal,
                    std::function<Clock::time_point()> now = Clock::now);

    [[nodiscard]] std::string_view name() const noexcept override { return "limits"; }
    [[nodiscard]] Decision authorize(const ops::Caller& caller, net::Message& request) override;
    void audit(const AuditRecord& record) override;

    [[nodiscard]] std::uint32_t open_streams(const ops::Caller& caller) const;

private:
    struct Account {
        double tokens{0};
        Clock::time_point refilled{};
        std::uint32_t streams{0};
    };
    [[nodiscard]] static std::string key_of(const ops::Caller& caller);

    std::uint32_t calls_per_second_;
    std::uint32_t streams_per_principal_;
    std::function<Clock::time_point()> now_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Account> accounts_;
};

} // namespace modb::proxy
