#pragma once

// Única porta de entrada do módulo no banco (Fase 9 / ADR-012).

#include "modb/ops/caller.hpp"
#include "modb/ops/logger.hpp"
#include "modb/ops/object_access.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>

namespace modb::ops {

class ExecutionContext {
public:
    ExecutionContext(ObjectAccess objects, Logger& logger,
                     std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt,
                     const Caller* caller = nullptr) noexcept
        : objects_{std::move(objects)}, logger_{&logger}, deadline_{deadline},
          caller_{caller != nullptr ? caller : &anonymous_caller()} {}

    [[nodiscard]] ObjectAccess& objects() noexcept { return objects_; }
    [[nodiscard]] const ObjectAccess& objects() const noexcept { return objects_; }
    [[nodiscard]] Logger& logger() noexcept { return *logger_; }
    // Quem chamou (a sessão aberta pelo proxy); anônimo sem proxy.
    [[nodiscard]] const Caller& caller() const noexcept { return *caller_; }

    // Disponível apenas em modo read_write (há Transaction ativa).
    [[nodiscard]] object::Transaction& transaction() { return objects_.transaction(); }
    [[nodiscard]] bool writable() const noexcept { return objects_.writable(); }
    // Prazo da chamada (OperationRegistry::set_time_limit). A checagem é
    // cooperativa: quem acessa o banco confere antes (modb::server::Context).
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> deadline() const noexcept { return deadline_; }
    [[nodiscard]] bool past_deadline() const noexcept {
        return deadline_ && std::chrono::steady_clock::now() > *deadline_;
    }
    // O detalhe de um erro da chamada (minor 3, ADR-029): um Value codificado
    // que vai no OpResult junto com o código. Descartado se a chamada der certo.
    void set_error_detail(std::vector<std::byte> detail) { error_detail_ = std::move(detail); }
    [[nodiscard]] const std::vector<std::byte>& error_detail() const noexcept { return error_detail_; }

private:
    ObjectAccess objects_;
    Logger* logger_{nullptr};
    std::optional<std::chrono::steady_clock::time_point> deadline_{};
    const Caller* caller_{nullptr};
    std::vector<std::byte> error_detail_{};
};

} // namespace modb::ops
