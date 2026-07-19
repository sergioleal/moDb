#pragma once

// Exemplo obrigatório da Fase 9: transferência atômica entre contas.

#include "modb/ops/operation.hpp"
#include "modb/ops/module_manifest.hpp"
#include "modb/ops/operation_registry.hpp"
#include "modb/object/ids.hpp"
#include "modb/storage/binary.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace modb::examples {

struct Account {
    std::string owner{};
    std::int64_t balance{0};
};

class TransferFunds final : public ops::Operation {
public:
    static constexpr std::string_view k_id = "account.transfer";
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_write;

    TransferFunds(object::ObjectId source, object::ObjectId destination, std::int64_t amount)
        : source_{source}, destination_{destination}, amount_{amount} {}

    [[nodiscard]] std::string_view id() const noexcept override { return k_id; }
    [[nodiscard]] ops::OperationMode mode() const noexcept override { return k_mode; }

    [[nodiscard]] static Result<std::unique_ptr<ops::Operation>> decode(
        std::span<const std::byte> args);

    [[nodiscard]] static Result<std::vector<std::byte>> encode_args(object::ObjectId source,
                                                                    object::ObjectId destination,
                                                                    std::int64_t amount);

    [[nodiscard]] Result<ops::OperationResult> execute(ops::ExecutionContext& context) override;

private:
    object::ObjectId source_{};
    object::ObjectId destination_{};
    std::int64_t amount_{0};
};

// Operação que lança exceção (teste de rollback).
class ThrowingOperation final : public ops::Operation {
public:
    static constexpr std::string_view k_id = "test.throw";
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_write;

    [[nodiscard]] std::string_view id() const noexcept override { return k_id; }

    [[nodiscard]] static Result<std::unique_ptr<ops::Operation>> decode(
        std::span<const std::byte>) {
        return std::unique_ptr<ops::Operation>{new ThrowingOperation{}};
    }

    [[nodiscard]] Result<ops::OperationResult> execute(ops::ExecutionContext&) override {
        throw std::runtime_error("module boom");
    }
};

// Migração como Operation: incrementa balance de todas as contas listadas nos args.
class MigrationBumpBalance final : public ops::Operation {
public:
    static constexpr std::string_view k_id = "migration.bump_balance";
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_write;

    explicit MigrationBumpBalance(std::vector<object::ObjectId> ids, std::int64_t delta)
        : ids_{std::move(ids)}, delta_{delta} {}

    [[nodiscard]] std::string_view id() const noexcept override { return k_id; }

    [[nodiscard]] static Result<std::unique_ptr<ops::Operation>> decode(
        std::span<const std::byte> args);

    [[nodiscard]] static Result<std::vector<std::byte>> encode_args(
        const std::vector<object::ObjectId>& ids, std::int64_t delta);

    [[nodiscard]] Result<ops::OperationResult> execute(ops::ExecutionContext& context) override;

private:
    std::vector<object::ObjectId> ids_;
    std::int64_t delta_{0};
};

[[nodiscard]] ops::ModuleManifest transfer_funds_manifest(object::BaselineId baseline);
[[nodiscard]] Result<void> register_transfer_funds_module(ops::OperationRegistry& registry);

} // namespace modb::examples
