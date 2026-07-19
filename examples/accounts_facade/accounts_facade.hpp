#pragma once

// Exemplo da Fase 11D: facade Accounts sobre TransferFunds (módulo + rede).

#include "examples/transfer_funds/transfer_funds.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/facade_descriptor.hpp"
#include "modb/ops/module_manifest.hpp"
#include "modb/ops/operation_registry.hpp"

#include <cstdint>
#include <string_view>

namespace modb::examples {

// Tipagem do consumidor para Client::open_facade<AccountsFacade>() /
// ops::open_facade<AccountsFacade>(...).
struct AccountsFacade {
    static constexpr std::string_view k_id = "accounts";
    static constexpr std::uint32_t k_version = 1;
};

[[nodiscard]] ops::FacadeDescriptor accounts_facade_descriptor();

// Manifesto do módulo com facade `accounts` v1 (somente account.transfer).
[[nodiscard]] ops::ModuleManifest accounts_facade_manifest(object::BaselineId baseline);

// Registra as operações do módulo (reutiliza transfer_funds).
[[nodiscard]] Result<void> register_accounts_facade_module(ops::OperationRegistry& registry);

} // namespace modb::examples
