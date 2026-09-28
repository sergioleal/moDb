#pragma once

// Registro e despacho de operações de domínio (Fase 9).

#include "modb/error.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/caller.hpp"
#include "modb/ops/logger.hpp"
#include "modb/ops/operation.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace modb::ops {

class OperationRegistry {
public:
    void set_logger(Logger& logger) noexcept { logger_ = &logger; }

    // Uma chamada concluída, para log e métricas (S5.2).
    struct CallRecord {
        std::string_view id;
        std::optional<OperationMode> mode{};  // vazio = proc desconhecida
        std::chrono::nanoseconds duration{};
        const Error* error{nullptr};  // nullptr = sucesso
        const Caller* caller{nullptr};  // nunca nulo no observador
    };
    void set_call_observer(std::function<void(const CallRecord&)> observer) { observer_ = std::move(observer); }
    // Tempo máximo por chamada (0 = sem limite). Cooperativo: a operação só é
    // interrompida quando confere o prazo (modb::server::Context faz isso a
    // cada acesso ao banco); estourou, a chamada falha com operation_timeout
    // e a transação é desfeita (S5.4).
    void set_time_limit(std::chrono::milliseconds limit) noexcept { time_limit_ = limit; }

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

    // begin → execute → commit; erro/exceção → rollback. `caller` = quem chama
    // (nulo = anônimo), visível à operação por `ExecutionContext::caller()`.
    [[nodiscard]] Result<OperationResult> dispatch(std::string_view id,
                                                   std::span<const std::byte> args,
                                                   object::Database& database,
                                                   const Caller* caller = nullptr);

private:
    struct Entry {
        OperationFactory factory{nullptr};
        OperationMode mode{OperationMode::read_write};
    };

    std::unordered_map<std::string, Entry> factories_;
    NullLogger null_logger_{};
    Logger* logger_{&null_logger_};
    std::function<void(const CallRecord&)> observer_{};
    std::chrono::milliseconds time_limit_{0};

    [[nodiscard]] Result<OperationResult> dispatch_unobserved(std::string_view id, std::span<const std::byte> args,
                                                              object::Database& database,
                                                              const Caller& caller,
                                                              std::optional<OperationMode>& mode);
};

} // namespace modb::ops
