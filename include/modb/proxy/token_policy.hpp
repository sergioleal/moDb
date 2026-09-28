#pragma once

// Autenticação por token no proxy (ADR-028, PLANO_PROXY X6.2).
//
// O arquivo de tokens guarda só o SHA-256 de cada token, uma linha por cliente:
//
//   # comentário
//   sha256:<64 hex> <principal> [role1,role2,...]
//
// `modb-proxy hash-token <token> <principal> [roles]` imprime a linha. O
// cliente manda o token em claro no `Authenticate` (mecanismo "token"): o link
// do cliente até o proxy precisa de TLS (X8) fora de uma rede confiável.

#include "modb/error.hpp"
#include "modb/proxy/policy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace modb::proxy {

using Sha256 = std::array<std::uint8_t, 32>;

[[nodiscard]] Sha256 sha256(std::span<const std::byte> data) noexcept;
[[nodiscard]] Sha256 sha256(std::string_view text) noexcept;
[[nodiscard]] std::string to_hex(const Sha256& digest);

// A linha do arquivo de tokens para um token.
[[nodiscard]] std::string token_line(std::string_view token, std::string_view principal,
                                     std::span<const std::string> roles = {});

class TokenStore {
public:
    [[nodiscard]] static Result<TokenStore> parse(std::string_view text);
    [[nodiscard]] static Result<TokenStore> load(const std::filesystem::path& file);

    // Quem tem este token (compara com todas as entradas, sem sair cedo).
    [[nodiscard]] std::optional<ops::Caller> find(std::span<const std::byte> token) const;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        Sha256 hash{};
        std::string principal;
        std::vector<std::string> roles;
    };
    std::vector<Entry> entries_;
};

// Exige um token válido e passa o resto (autorização, respostas, auditoria)
// à política de dentro.
class TokenPolicy final : public Policy {
public:
    TokenPolicy(TokenStore tokens, std::shared_ptr<Policy> inner);

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] std::vector<std::string> mechanisms() const override { return {"token"}; }
    [[nodiscard]] Result<ops::Caller> authenticate(const ClientInfo& client, const Credentials& credentials) override;
    [[nodiscard]] Decision authorize(const ops::Caller& caller, net::Message& request) override;
    void on_response(const ops::Caller& caller, net::Message& response) override;
    void audit(const AuditRecord& record) override;

private:
    TokenStore tokens_;
    std::shared_ptr<Policy> inner_;
    std::string name_;
};

} // namespace modb::proxy
