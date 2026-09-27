#include "modb/ops/operation_registry.hpp"

#include "modb/ops/execution_context.hpp"
#include "modb/ops/object_access.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace modb::ops {

Result<void> OperationRegistry::register_factory(std::string id, OperationFactory factory,
                                                 OperationMode mode) {
    if (id.empty()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "operation id is empty"});
    }
    if (factory == nullptr) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "operation factory is null"});
    }
    if (factories_.contains(id)) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "operation id already registered: " + id});
    }
    factories_.emplace(std::move(id), Entry{.factory = factory, .mode = mode});
    return {};
}

bool OperationRegistry::contains(std::string_view id) const {
    return factories_.contains(std::string{id});
}

Result<OperationResult> OperationRegistry::dispatch(std::string_view id,
                                                    std::span<const std::byte> args,
                                                    object::Database& database) {
    const auto start = std::chrono::steady_clock::now();
    std::optional<OperationMode> mode;
    auto result = dispatch_unobserved(id, args, database, mode);
    if (observer_) {
        const CallRecord record{.id = id,
                                .mode = mode,
                                .duration = std::chrono::steady_clock::now() - start,
                                .error = result ? nullptr : &result.error()};
        try {
            observer_(record);
        } catch (...) {
            // Observador (log) não pode derrubar a chamada.
        }
    }
    return result;
}

namespace {

Error timeout_error(std::chrono::milliseconds limit) {
    return Error{ErrorCode::operation_timeout,
                 "operation exceeded the time limit of " + std::to_string(limit.count()) + " ms"};
}

} // namespace

Result<OperationResult> OperationRegistry::dispatch_unobserved(std::string_view id,
                                                               std::span<const std::byte> args,
                                                               object::Database& database,
                                                               std::optional<OperationMode>& mode) {
    const auto found = factories_.find(std::string{id});
    if (found == factories_.end()) {
        return std::unexpected(
            Error{ErrorCode::operation_not_found, "operation not found: " + std::string{id}});
    }
    mode = found->second.mode;

    auto operation = found->second.factory(args);
    if (!operation) {
        return std::unexpected(operation.error());
    }

    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (time_limit_.count() > 0) {
        deadline = std::chrono::steady_clock::now() + time_limit_;
    }

    Logger& logger = *logger_;
    try {
        if (found->second.mode == OperationMode::read_only) {
            auto snap = database.snapshot();
            if (!snap) {
                return std::unexpected(snap.error());
            }
            ObjectAccess access{database, nullptr, &*snap};
            ExecutionContext context{std::move(access), logger, deadline};
            auto result = (*operation)->execute(context);
            if (context.past_deadline()) {
                return std::unexpected(timeout_error(time_limit_));
            }
            return result;
        }

        return database.transact([&](object::Transaction& tx) -> Result<OperationResult> {
            ObjectAccess access{database, &tx, nullptr};
            ExecutionContext context{std::move(access), logger, deadline};
            auto result = (*operation)->execute(context);
            // Passou do prazo mesmo tendo "dado certo": desfaz, para o
            // resultado não depender de onde a operação conferiu o relógio.
            if (context.past_deadline()) {
                return std::unexpected(timeout_error(time_limit_));
            }
            return result;
        });
    } catch (const std::exception& ex) {
        logger.error(ex.what());
        return std::unexpected(
            Error{ErrorCode::internal_error, std::string{"operation threw: "} + ex.what()});
    } catch (...) {
        logger.error("operation threw unknown exception");
        return std::unexpected(
            Error{ErrorCode::internal_error, "operation threw unknown exception"});
    }
}

} // namespace modb::ops
