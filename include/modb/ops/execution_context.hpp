#pragma once

// Única porta de entrada do módulo no banco (Fase 9 / ADR-012).

#include "modb/ops/logger.hpp"
#include "modb/ops/object_access.hpp"

namespace modb::ops {

class ExecutionContext {
public:
    ExecutionContext(ObjectAccess objects, Logger& logger) noexcept
        : objects_{std::move(objects)}, logger_{&logger} {}

    [[nodiscard]] ObjectAccess& objects() noexcept { return objects_; }
    [[nodiscard]] const ObjectAccess& objects() const noexcept { return objects_; }
    [[nodiscard]] Logger& logger() noexcept { return *logger_; }

    // Disponível apenas em modo read_write (há Transaction ativa).
    [[nodiscard]] object::Transaction& transaction() { return objects_.transaction(); }
    [[nodiscard]] bool writable() const noexcept { return objects_.writable(); }

private:
    ObjectAccess objects_;
    Logger* logger_{nullptr};
};

} // namespace modb::ops
