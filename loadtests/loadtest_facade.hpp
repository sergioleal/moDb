#pragma once

// Facade de rede da Subfase G (docs/PLANO_TESTES_DE_CARGA.md §4.3, alvo
// `loopback`): expõe só o suficiente para o workload `create_only` rodar
// contra um `modb::net::Server` de verdade em vez de um `Database` local --
// o motor não muda, só o transporte. Segue o mesmo molde de
// `examples/accounts_facade` (facade sobre uma `ops::Operation`).
//
// Versão mínima (Subfase G reduzida): só `CreateBatch`. `create_delete_*` e
// `crud_full` sobre `loopback` continuam "unimplemented" -- ver
// docs-process/PLANO_IMPLEMENTACAO_CARGA.md.

#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/facade_descriptor.hpp"
#include "modb/ops/module_manifest.hpp"
#include "modb/ops/operation.hpp"
#include "modb/ops/operation_registry.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace modb::loadtest {

// Tipagem do consumidor para `ServerConnection::open_facade<LoadtestFacade>()`.
struct LoadtestFacade {
    static constexpr std::string_view k_id = "loadtest";
    static constexpr std::uint32_t k_version = 1;
};

// Cria `count` `User`s determinados por (seed, index) -- `generate_user(seed,
// start_index+i, payload)` para i em [0,count) -- numa única transação
// (`ops::Operation::execute` roda dentro de `Database::transact`, igual ao
// commit em lote de `target_embedded.cpp::perform_create_phase`). Não
// devolve os `ObjectId`s gerados: a validação do lado cliente relê TUDO via
// `query`/`collect` no fim do caso, não precisa deles.
class CreateBatch final : public ops::Operation {
public:
    static constexpr std::string_view k_id = "loadtest.create_batch";
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_write;

    CreateBatch(std::uint64_t seed, std::uint64_t start_index, std::uint64_t count,
               std::string payload)
        : seed_{seed}, start_index_{start_index}, count_{count}, payload_{std::move(payload)} {}

    [[nodiscard]] std::string_view id() const noexcept override { return k_id; }
    [[nodiscard]] ops::OperationMode mode() const noexcept override { return k_mode; }

    [[nodiscard]] static Result<std::unique_ptr<ops::Operation>> decode(
        std::span<const std::byte> args);

    [[nodiscard]] static Result<std::vector<std::byte>> encode_args(std::uint64_t seed,
                                                                    std::uint64_t start_index,
                                                                    std::uint64_t count,
                                                                    std::string_view payload);

    [[nodiscard]] Result<ops::OperationResult> execute(ops::ExecutionContext& context) override;

private:
    std::uint64_t seed_{};
    std::uint64_t start_index_{};
    std::uint64_t count_{};
    std::string payload_;
};

[[nodiscard]] ops::FacadeDescriptor loadtest_facade_descriptor();
[[nodiscard]] ops::ModuleManifest loadtest_facade_manifest(object::BaselineId baseline);
[[nodiscard]] Result<void> register_loadtest_facade_module(ops::OperationRegistry& registry);

} // namespace modb::loadtest
