#include "modb/ops/operation_registry.hpp"

#include "modb/ops/execution_context.hpp"
#include "modb/ops/object_access.hpp"

#include <exception>
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
    const auto found = factories_.find(std::string{id});
    if (found == factories_.end()) {
        return std::unexpected(
            Error{ErrorCode::operation_not_found, "operation not found: " + std::string{id}});
    }

    auto operation = found->second.factory(args);
    if (!operation) {
        return std::unexpected(operation.error());
    }

    Logger& logger = *logger_;
    try {
        if (found->second.mode == OperationMode::read_only) {
            auto snap = database.snapshot();
            if (!snap) {
                return std::unexpected(snap.error());
            }
            ObjectAccess access{database, nullptr, &*snap};
            ExecutionContext context{std::move(access), logger};
            return (*operation)->execute(context);
        }

        return database.transact([&](object::Transaction& tx) -> Result<OperationResult> {
            ObjectAccess access{database, &tx, nullptr};
            ExecutionContext context{std::move(access), logger};
            return (*operation)->execute(context);
        });
    } catch (const std::exception& ex) {
        logger.error(ex.what());
        return std::unexpected(
            Error{ErrorCode::invalid_argument, std::string{"operation threw: "} + ex.what()});
    } catch (...) {
        logger.error("operation threw unknown exception");
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "operation threw unknown exception"});
    }
}

} // namespace modb::ops
