#pragma once

// Módulos de aplicação com procs declaradas como funções (PLANO_SERVIDOR_PROCS
// S3/S4, ADR-025).
//
//   modb::server::Module modb_module_biblioteca() {
//       return modb::server::ModuleBuilder{"biblioteca"}
//           .type(livro_binding())
//           .index<Livro>(kLivroIsbn)
//           .proc("livros.obter", Mode::read_only, "Um livro pelo id",
//                 [](Context& c, const ops::Args& a) -> Result<ops::Value> {
//                     auto id = a.id("id");
//                     if (!id) return std::unexpected(id.error());
//                     auto livro = c.read<Livro>(*id);
//                     ...
//                     return ops::Value::object({{"id", *id}, {"titulo", livro->titulo}});
//                 })
//           .build();
//   }
//
// Cada proc roda numa transação (escrita) ou num snapshot (leitura) aberto pelo
// OperationRegistry: devolver erro ou lançar exceção desfaz tudo que ela fez.
// Erros de regra com código: invalid(...), not_found(...), conflict(...).

#include "modb/error.hpp"
#include "modb/object/attribute_value.hpp"
#include "modb/object/blob_store.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/execution_context.hpp"
#include "modb/ops/operation.hpp"
#include "modb/ops/value.hpp"
#include "modb/server/host.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modb::server {

using Mode = ops::OperationMode;

// --- erros de regra (S3.2) -------------------------------------------------------

[[nodiscard]] inline Error invalid(std::string message) { return Error{ErrorCode::invalid_argument, std::move(message)}; }
[[nodiscard]] inline Error not_found(std::string message) { return Error{ErrorCode::record_not_found, std::move(message)}; }
[[nodiscard]] inline Error conflict(std::string message) { return Error{ErrorCode::conflict, std::move(message)}; }

namespace detail {
template <typename>
struct member_of;
template <typename C, typename F>
struct member_of<F C::*> {
    using type = C;
};
} // namespace detail

// --- o que uma proc enxerga do banco (S4) -------------------------------------------
//
// Numa proc de escrita, tudo acontece na transação da chamada (e as consultas
// veem o estado confirmado mais as escritas da própria chamada, pelo índice).
// Numa proc de leitura, tudo -- `read`, `where`/`all`, `find` -- usa o snapshot
// da chamada: procs de leitura correm em paralelo com commits de outras
// chamadas (C11) e mesmo assim cada uma vê um estado só.
class Context {
public:
    explicit Context(ops::ExecutionContext& context,
                     const std::map<std::string, std::string>* settings = nullptr) noexcept
        : context_{&context}, settings_{settings} {}

    // Valor de uma configuração declarada pelo módulo (ModuleBuilder::setting),
    // já validado na subida. Um nome não declarado é defeito da proc: lança, e a
    // chamada falha com `internal_error`, com a transação desfeita.
    [[nodiscard]] const std::string& setting(std::string_view name) const {
        if (settings_ != nullptr) {
            if (const auto found = settings_->find(std::string{name}); found != settings_->end()) {
                return found->second;
            }
        }
        throw std::out_of_range{"setting not declared by the module: " + std::string{name}};
    }

    [[nodiscard]] bool writable() const noexcept { return context_->writable(); }
    [[nodiscard]] object::Database& database() noexcept { return context_->objects().database(); }
    [[nodiscard]] ops::Logger& log() noexcept { return context_->logger(); }
    // Quem chamou: o principal, as roles e os atributos que o proxy deu na
    // abertura da sessão (ADR-028); anônimo sem proxy.
    [[nodiscard]] const ops::Caller& caller() const noexcept { return context_->caller(); }
    // Data de hoje (UTC) no relógio do servidor.
    [[nodiscard]] static std::chrono::sys_days today() {
        return std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    }

    // --- leitura ---
    template <typename T>
    [[nodiscard]] Result<T> read(object::ObjectId id) {
        if (auto ok = in_time(); !ok) {
            return std::unexpected(ok.error());
        }
        return context_->objects().read<T>(id);
    }

    // Todos os objetos do tipo que satisfazem `predicate` (todos, se vazio), com os ids.
    template <typename T>
    [[nodiscard]] Result<std::vector<std::pair<object::ObjectId, T>>> where(std::function<bool(const T&)> predicate = {}) {
        if (auto ok = in_time(); !ok) {
            return std::unexpected(ok.error());
        }
        auto query = open_query<T>();
        std::vector<object::ObjectId> ids;
        auto rows = predicate ? std::move(query).where(std::move(predicate)).select({object::FieldId{0}}).stream()
                              : std::move(query).select({object::FieldId{0}}).stream();
        for (auto& row : rows) {
            if (auto ok = in_time(); !ok) {
                return std::unexpected(ok.error());
            }
            if (!row) {
                return std::unexpected(row.error());
            }
            const auto field = row->get(object::FieldId{0});
            if (!field) {
                return std::unexpected(Error{ErrorCode::field_not_found, "query row without id"});
            }
            auto id = field->as_ref();
            if (!id) {
                return std::unexpected(id.error());
            }
            ids.push_back(*id);
        }
        std::vector<std::pair<object::ObjectId, T>> out;
        out.reserve(ids.size());
        for (const auto id : ids) {
            auto value = read<T>(id);
            if (!value) {
                return std::unexpected(value.error());
            }
            out.emplace_back(id, std::move(*value));
        }
        return out;
    }
    template <typename T>
    [[nodiscard]] Result<std::vector<std::pair<object::ObjectId, T>>> all() {
        return where<T>();
    }

    // Ids com `field == value`, pelo índice (o campo precisa de índice: ModuleBuilder::index).
    template <typename T>
    [[nodiscard]] Result<std::vector<object::ObjectId>> find(object::FieldId field, object::AttributeValue value) {
        if (auto ok = in_time(); !ok) {
            return std::unexpected(ok.error());
        }
        if (writable()) {
            // Na transação: o índice corrente, que inclui o que esta chamada já escreveu.
            return database().indexed_object_ids<T>(field, std::move(value));
        }
        // Proc de leitura: pelo índice, mas na época do snapshot da chamada.
        std::vector<object::ObjectId> ids;
        for (auto& row : open_query<T>().equals(field, std::move(value)).select({object::FieldId{0}}).stream()) {
            if (!row) {
                return std::unexpected(row.error());
            }
            const auto id_field = row->get(object::FieldId{0});
            auto id = id_field ? id_field->as_ref() : Result<object::ObjectId>{std::unexpected(Error{ErrorCode::field_not_found, "query row without id"})};
            if (!id) {
                return std::unexpected(id.error());
            }
            ids.push_back(*id);
        }
        return ids;
    }

    // --- escrita (só em procs Mode::read_write) ---
    template <typename T>
    [[nodiscard]] Result<object::ObjectId> create(const T& value) {
        if (auto ok = in_time(); !ok) {
            return std::unexpected(ok.error());
        }
        auto handle = context_->objects().create(value);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        return handle->id();
    }
    template <typename T>
    [[nodiscard]] Result<void> update(object::ObjectId id, const T& value) {
        if (auto ok = in_time(); !ok) {
            return ok;
        }
        auto handle = context_->objects().get<T>(id);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        return context_->objects().update(*handle, value);
    }
    // Muda um campo: c.set<&Exemplar::estado>(id, std::string{"emprestado"}).
    template <auto Member, typename V>
    [[nodiscard]] Result<void> set(object::ObjectId id, V&& value) {
        using T = typename detail::member_of<decltype(Member)>::type;
        if (!writable()) {
            return std::unexpected(Error{ErrorCode::transaction_required, "set requires a read_write proc"});
        }
        if (auto ok = in_time(); !ok) {
            return ok;
        }
        auto handle = context_->objects().get<T>(id);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        return handle->template set<Member>(context_->transaction(), std::forward<V>(value));
    }
    [[nodiscard]] Result<void> remove(object::ObjectId id) {
        if (auto ok = in_time(); !ok) {
            return ok;
        }
        return context_->objects().remove(id);
    }

    // Erro operation_timeout se a chamada passou do tempo limite do servidor
    // (--proc-timeout-ms). Todo acesso ao banco confere; uma proc que calcula
    // muito sem tocar no banco pode conferir por conta própria.
    [[nodiscard]] Result<void> in_time() const {
        if (context_->past_deadline()) {
            return std::unexpected(Error{ErrorCode::operation_timeout, "proc exceeded the server time limit"});
        }
        return {};
    }

    // Coleções persistentes (PersistentVector/Set/Map) e blobs: passe
    // `blobs()` e `transaction()` às APIs de collection.hpp.
    [[nodiscard]] object::BlobStore blobs() { return database().blobs(); }
    [[nodiscard]] object::Transaction& transaction() { return context_->transaction(); }

private:
    // Consulta na época da chamada: o snapshot dela numa proc de leitura; o
    // estado confirmado numa de escrita.
    template <typename T>
    [[nodiscard]] object::Query<T> open_query() {
        if (auto* snapshot = context_->objects().snapshot()) {
            return database().query<T>(*snapshot);
        }
        return database().query<T>();
    }

    ops::ExecutionContext* context_;
    const std::map<std::string, std::string>* settings_;
};

using ProcFn = std::function<Result<ops::Value>(Context&, const ops::Args&)>;

// --- montagem de um módulo (S3.1, S4.3) ---------------------------------------------------

class ModuleBuilder {
public:
    explicit ModuleBuilder(std::string id, std::uint32_t version = 1) : id_{std::move(id)}, version_{version} {}

    // Tipo persistido pelo módulo: bind na abertura do servidor.
    template <typename T>
    ModuleBuilder& type(object::BindingBuilder<T> binding) {
        auto shared = std::make_shared<object::BindingBuilder<T>>(std::move(binding));
        preparers_.push_back([shared](object::Database& db) { return db.bind(*shared); });
        return *this;
    }
    // Índice B+ tree num campo do tipo (criado na primeira abertura; já existir não é erro).
    template <typename T>
    ModuleBuilder& index(object::FieldId field) {
        preparers_.push_back([field](object::Database& db) -> Result<void> {
            auto created = db.create_index<T>(field);
            if (!created && created.error().message.find("index already exists") == std::string::npos) {
                return created;
            }
            return {};
        });
        return *this;
    }
    ModuleBuilder& proc(std::string name, Mode mode, std::string description, ProcFn fn);
    // Configuração da aplicação: `<módulo>.<nome> = valor` no `.conf` ou
    // `--<módulo>.<nome> valor`; a proc lê com `c.setting(nome)`. `validate`
    // recusa um valor inválido, e o servidor não sobe.
    ModuleBuilder& setting(std::string name, std::string default_value, std::string description,
                           std::function<Result<void>(std::string_view)> validate = {});

    [[nodiscard]] Module build() const;

private:
    struct Proc {
        std::string name;
        Mode mode;
        std::string description;
        std::shared_ptr<ProcFn> fn;
    };
    std::string id_;
    std::uint32_t version_;
    std::vector<std::function<Result<void>(object::Database&)>> preparers_;
    std::vector<Proc> procs_;
    std::vector<ModuleSetting> settings_;
};

} // namespace modb::server
