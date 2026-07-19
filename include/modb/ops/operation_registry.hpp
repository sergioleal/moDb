#pragma once

// Registro e despacho de operações de domínio (Fase 9).

#include "modb/error.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/logger.hpp"
#include "modb/ops/operation.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace modb::ops {

class OperationRegistry {
public:
    void set_logger(Logger& logger) noexcept { logger_ = &logger; }

    [[nodiscard]] Result<void> register_factory(std::string id, OperationFactory factory,
                                                OperationMode mode = OperationMode::read_write);

    template <typename Op>
    [[nodiscard]] Result<void> register_operation(std::string id) {
        return register_factory(
            std::move(id),
            [](std::span<const std::byte> args) -> Result<std::unique_ptr<Operation>> {
                return Op::decode(args);
            },
            Op::k_mode);
    }

    [[nodiscard]] bool contains(std::string_view id) const;
    [[nodiscard]] std::size_t size() const noexcept { return factories_.size(); }

    // begin → execute → commit; erro/exceção → rollback.
    [[nodiscard]] Result<OperationResult> dispatch(std::string_view id,
                                                   std::span<const std::byte> args,
                                                   object::Database& database);

private:
    struct Entry {
        OperationFactory factory{nullptr};
        OperationMode mode{OperationMode::read_write};
    };

    std::unordered_map<std::string, Entry> factories_;
    NullLogger null_logger_{};
    Logger* logger_{&null_logger_};
};

} // namespace modb::ops
