#pragma once

// Registro e despacho de operações de domínio (Fase 9).

#include "modb/error.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/caller.hpp"
#include "modb/ops/logger.hpp"
#include "modb/ops/operation.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace modb::ops {

// O que vai junto com uma chamada, além dos argumentos (minor 3, ADR-029).
struct CallExtras {
    // Chave de idempotência: numa proc de escrita, o resultado de uma chamada
    // já confirmada com a mesma chave (do mesmo principal) volta sem executar
    // de novo. Exige `enable_idempotency`.
    std::string_view idempotency_key{};
    // Recebe o detalhe do erro que a proc mandou (vazio sem erro ou sem detalhe).
    std::vector<std::byte>* error_detail{nullptr};
};

// Registro de uma chamada com chave (tipo de sistema "sys.Idempotency").
struct IdempotencyRecord {
    // principal + '\n' + chave: a mesma chave de dois principais são duas.
    std::string scope{};
    std::vector<std::byte> result{};
    std::int64_t created_ms{0};
    // 0 = o resultado passou do limite e não foi guardado (só a chave).
    std::int64_t stored{1};
};

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
        // Detalhe do erro (Value codificado; vazio = nenhum), minor 3.
        std::span<const std::byte> error_detail{};
        // A chamada tinha chave e o resultado veio do registro, sem executar.
        bool replayed{false};
    };
    void set_call_observer(std::function<void(const CallRecord&)> observer) { observer_ = std::move(observer); }
    // Tempo máximo por chamada (0 = sem limite). Cooperativo: a operação só é
    // interrompida quando confere o prazo (modb::server::Context faz isso a
    // cada acesso ao banco); estourou, a chamada falha com operation_timeout
    // e a transação é desfeita (S5.4).
    void set_time_limit(std::chrono::milliseconds limit) noexcept { time_limit_ = limit; }

    // Chaves de idempotência (ADR-029): liga o tipo de sistema "sys.Idempotency"
    // no banco (bind e índices; sem transação aberta, como o `prepare` dos
    // módulos). Cada escrita com chave apaga até 16 registros mais velhos que
    // `retention`. Sem isto, uma chamada com chave é `invalid_argument`.
    [[nodiscard]] Result<void> enable_idempotency(object::Database& database,
                                                  std::chrono::seconds retention = std::chrono::hours{24});
    [[nodiscard]] bool idempotency_enabled() const noexcept { return idempotency_; }
    // Maior resultado que o registro guarda: o objeto precisa caber numa página.
    static constexpr std::size_t k_idempotency_max_result = 4096;

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
    // As operações registradas e o modo de cada uma, em ordem de id (o
    // catálogo que o engine passa aos proxies, ADR-028).
    [[nodiscard]] std::vector<std::pair<std::string, OperationMode>> list() const;
    [[nodiscard]] std::size_t size() const noexcept { return factories_.size(); }

    // begin → execute → commit; erro/exceção → rollback. `caller` = quem chama
    // (nulo = anônimo), visível à operação por `ExecutionContext::caller()`.
    [[nodiscard]] Result<OperationResult> dispatch(std::string_view id,
                                                   std::span<const std::byte> args,
                                                   object::Database& database,
                                                   const Caller* caller = nullptr);
    [[nodiscard]] Result<OperationResult> dispatch(std::string_view id, std::span<const std::byte> args,
                                                   object::Database& database, const Caller* caller,
                                                   const CallExtras& extras);

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
    bool idempotency_{false};
    std::chrono::seconds idempotency_retention_{std::chrono::hours{24}};

    [[nodiscard]] Result<OperationResult> dispatch_unobserved(std::string_view id, std::span<const std::byte> args,
                                                              object::Database& database,
                                                              const Caller& caller,
                                                              std::optional<OperationMode>& mode,
                                                              const CallExtras& extras, std::vector<std::byte>& detail,
                                                              bool& replayed);
};

} // namespace modb::ops
