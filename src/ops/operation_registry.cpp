#include "modb/ops/operation_registry.hpp"

#include <algorithm>

#include "modb/ops/execution_context.hpp"
#include "modb/ops/object_access.hpp"
#include "modb/ops/value.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace modb::ops {

namespace {

// Campos indexados do tipo de sistema "sys.Idempotency" (ADR-029): 1 scope,
// 2 result, 3 created_ms, 4 stored. Estáveis: estão no arquivo.
constexpr object::FieldId k_idem_scope{1};
constexpr object::FieldId k_idem_created{3};
// Registros vencidos apagados por escrita com chave.
constexpr std::size_t k_idem_purge_per_write = 16;

object::BindingBuilder<IdempotencyRecord> idempotency_binding() {
    object::BindingBuilder<IdempotencyRecord> b{"sys.Idempotency"};
    b.field<1>("scope", &IdempotencyRecord::scope)
        .field<2>("result", &IdempotencyRecord::result)
        .field<3>("created_ms", &IdempotencyRecord::created_ms)
        .field<4>("stored", &IdempotencyRecord::stored);
    return b;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

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

Result<void> OperationRegistry::enable_idempotency(object::Database& database, std::chrono::seconds retention) {
    if (auto bound = database.bind(idempotency_binding()); !bound) {
        return std::unexpected(bound.error());
    }
    for (const auto field : {k_idem_scope, k_idem_created}) {
        auto created = database.create_index<IdempotencyRecord>(field);
        if (!created && created.error().message.find("index already exists") == std::string::npos) {
            return created;
        }
    }
    idempotency_ = true;
    idempotency_retention_ = retention;
    return {};
}

bool OperationRegistry::contains(std::string_view id) const {
    return factories_.contains(std::string{id});
}

std::vector<std::pair<std::string, OperationMode>> OperationRegistry::list() const {
    std::vector<std::pair<std::string, OperationMode>> out;
    out.reserve(factories_.size());
    for (const auto& [id, entry] : factories_) {
        out.emplace_back(id, entry.mode);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Result<OperationResult> OperationRegistry::dispatch(std::string_view id,
                                                    std::span<const std::byte> args,
                                                    object::Database& database,
                                                    const Caller* caller) {
    return dispatch(id, args, database, caller, CallExtras{});
}

Result<OperationResult> OperationRegistry::dispatch(std::string_view id, std::span<const std::byte> args,
                                                    object::Database& database, const Caller* caller,
                                                    const CallExtras& extras) {
    const Caller& who = caller != nullptr ? *caller : anonymous_caller();
    const auto start = std::chrono::steady_clock::now();
    std::optional<OperationMode> mode;
    std::vector<std::byte> detail;
    bool replayed = false;
    auto result = dispatch_unobserved(id, args, database, who, mode, extras, detail, replayed);
    if (result) {
        detail.clear();
    }
    if (observer_) {
        const CallRecord record{.id = id,
                                .mode = mode,
                                .duration = std::chrono::steady_clock::now() - start,
                                .error = result ? nullptr : &result.error(),
                                .caller = &who,
                                .error_detail = detail,
                                .replayed = replayed};
        try {
            observer_(record);
        } catch (...) {
            // Observador (log) não pode derrubar a chamada.
        }
    }
    if (extras.error_detail != nullptr) {
        *extras.error_detail = std::move(detail);
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
                                                               const Caller& caller,
                                                               std::optional<OperationMode>& mode,
                                                               const CallExtras& extras, std::vector<std::byte>& detail,
                                                               bool& replayed) {
    const auto found = factories_.find(std::string{id});
    if (found == factories_.end()) {
        return std::unexpected(
            Error{ErrorCode::operation_not_found, "operation not found: " + std::string{id}});
    }
    mode = found->second.mode;
    const bool keyed = !extras.idempotency_key.empty() && found->second.mode == OperationMode::read_write;
    if (keyed && !idempotency_) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "idempotency keys are not enabled on this server"});
    }

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
            // O lock de leitura ANTES do snapshot, e por toda a chamada (ADR-027):
            // com o snapshot aberto enquanto esperava o lock atrás de escritores,
            // cada um deles achava um snapshot mais antigo que o último commit e
            // batia em snapshot_conflict. Assim, quando uma escrita entra, o
            // snapshot desta chamada já fechou. Procs são curtas. A chave de
            // idempotência não vale aqui: uma leitura pode ser repetida.
            const auto read_lock = database.read_guard();
            auto snap = database.snapshot();
            if (!snap) {
                return std::unexpected(snap.error());
            }
            ObjectAccess access{database, nullptr, &*snap};
            ExecutionContext context{std::move(access), logger, deadline, &caller};
            auto result = (*operation)->execute(context);
            if (context.past_deadline()) {
                return std::unexpected(timeout_error(time_limit_));
            }
            if (!result) {
                detail = context.error_detail();
            }
            return result;
        }

        const std::string scope = keyed ? caller.principal + '\n' + std::string{extras.idempotency_key} : std::string{};
        return database.transact([&](object::Transaction& tx) -> Result<OperationResult> {
            ObjectAccess access{database, &tx, nullptr};
            replayed = false;
            detail.clear();
            if (keyed) {
                // Na mesma transação da escrita (ADR-029): a chave só existe se
                // a escrita foi confirmada, inclusive depois de uma queda.
                auto seen = database.indexed_object_ids<IdempotencyRecord>(k_idem_scope, object::AttributeValue{scope});
                if (!seen) {
                    return std::unexpected(seen.error());
                }
                if (!seen->empty()) {
                    auto record = access.read<IdempotencyRecord>(seen->front());
                    if (!record) {
                        return std::unexpected(record.error());
                    }
                    replayed = true;
                    if (record->stored == 0) {
                        detail = encode(Value::object({{"reason", std::string{"idempotent_result_too_large"}}}));
                        return std::unexpected(Error{ErrorCode::conflict,
                                                     "this call already ran with this idempotency key; its result "
                                                     "was too large to keep"});
                    }
                    return OperationResult{.payload = std::move(record->result)};
                }
            }
            ExecutionContext context{std::move(access), logger, deadline, &caller};
            auto result = (*operation)->execute(context);
            // Passou do prazo mesmo tendo "dado certo": desfaz, para o
            // resultado não depender de onde a operação conferiu o relógio.
            if (context.past_deadline()) {
                return std::unexpected(timeout_error(time_limit_));
            }
            if (!result) {
                detail = context.error_detail();
                return result;
            }
            if (keyed) {
                ObjectAccess writer{database, &tx, nullptr};
                const auto now = now_ms();
                const auto cutoff = now - std::chrono::duration_cast<std::chrono::milliseconds>(idempotency_retention_).count();
                auto expired = database.indexed_object_ids_between<IdempotencyRecord>(
                    k_idem_created, object::AttributeValue{std::int64_t{0}}, object::AttributeValue{cutoff});
                if (!expired) {
                    return std::unexpected(expired.error());
                }
                for (std::size_t i = 0; i < expired->size() && i < k_idem_purge_per_write; ++i) {
                    if (auto removed = writer.remove((*expired)[i]); !removed) {
                        return std::unexpected(removed.error());
                    }
                }
                const bool fits = result->payload.size() <= k_idempotency_max_result;
                IdempotencyRecord record{.scope = scope,
                                         .result = fits ? result->payload : std::vector<std::byte>{},
                                         .created_ms = now,
                                         .stored = fits ? 1 : 0};
                if (auto created = writer.create(record); !created) {
                    return std::unexpected(created.error());
                }
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
