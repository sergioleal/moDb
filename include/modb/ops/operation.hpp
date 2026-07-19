#pragma once

// Contrato de operação de domínio (Fase 9).

#include "modb/error.hpp"
#include "modb/ops/execution_context.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace modb::ops {

struct OperationResult {
    std::vector<std::byte> payload{};

    friend bool operator==(const OperationResult&, const OperationResult&) = default;
};

enum class OperationMode : std::uint8_t {
    read_only = 0,
    read_write = 1,
};

class Operation {
public:
    virtual ~Operation() = default;

    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    [[nodiscard]] virtual OperationMode mode() const noexcept {
        return OperationMode::read_write;
    }

    // Fronteira do motor: preferir Result. Exceções que escapem são capturadas
    // pelo registry → rollback + erro.
    [[nodiscard]] virtual Result<OperationResult> execute(ExecutionContext& context) = 0;
};

using OperationFactory =
    Result<std::unique_ptr<Operation>> (*)(std::span<const std::byte> args);

} // namespace modb::ops
