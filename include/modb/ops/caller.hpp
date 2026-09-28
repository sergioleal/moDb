#pragma once

// Quem chama uma operação (ADR-028, PLANO_PROXY X4.5).
//
// O proxy autentica o cliente e abre a sessão no engine dizendo quem ele é
// (`SessionOpen`); a identidade chega à proc por `ExecutionContext::caller()`.
// O engine não confere credenciais: confia no link. Uma chamada sem proxy
// (modo TCP direto, embutido) tem o chamador anônimo.

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modb::ops {

struct Caller {
    // Vazio = anônimo.
    std::string principal{};
    std::vector<std::string> roles{};
    // Atributos da sessão dados pelo proxy (ex.: "ip", "tls").
    std::vector<std::pair<std::string, std::string>> attributes{};

    [[nodiscard]] bool anonymous() const noexcept { return principal.empty(); }
    [[nodiscard]] bool has_role(std::string_view role) const noexcept {
        return std::find(roles.begin(), roles.end(), role) != roles.end();
    }
    [[nodiscard]] std::string_view attribute(std::string_view key) const noexcept {
        for (const auto& [name, value] : attributes) {
            if (name == key) {
                return value;
            }
        }
        return {};
    }

    friend bool operator==(const Caller&, const Caller&) = default;
};

// O chamador anônimo, para quem não tem sessão identificada.
[[nodiscard]] inline const Caller& anonymous_caller() noexcept {
    static const Caller anonymous{};
    return anonymous;
}

} // namespace modb::ops
