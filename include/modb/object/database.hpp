#pragma once

// Importa Result e os códigos de erro.
#include "modb/error.hpp"
// Importa Binding/BindingBuilder.
#include "modb/object/binding.hpp"
// Importa o BlobStore exposto para coleções e binários grandes.
#include "modb/object/blob_store.hpp"
// Importa Handle.
#include "modb/object/handle.hpp"
// Importa ObjectStore (usado pelos métodos template inline).
#include "modb/object/object_store.hpp"
// Importa ProjectionPlan para materializar versões históricas.
#include "modb/object/projection_plan.hpp"
// Importa o gerador e os operadores de streaming (Fase 7A).
#include "modb/query/generator.hpp"
#include "modb/query/operators.hpp"
#include "modb/query/projected_row.hpp"
// Importa PageFile, mantido por ponteiro estável.
#include "modb/storage/page_file.hpp"
// Importa a fábrica do WAL (injeção de failpoint) usada no commit.
#include "modb/tx/wal.hpp"

// Disponibiliza std::size_t no resultado do GC.
#include <cstddef>
// Disponibiliza caminhos.
#include <filesystem>
// Disponibiliza callbacks de migração.
#include <functional>
// Disponibiliza a posse estável do PageFile.
#include <memory>
// Disponibiliza sincronização do registro global.
#include <mutex>
// Disponibiliza nomes de tipos nas migrações.
#include <string>
#include <vector>
// Disponibiliza std::optional na época de snapshot mais antiga.
#include <optional>
// Disponibiliza o registro de épocas de snapshots abertos.
#include <set>
// Disponibiliza o mapa de bindings.
#include <unordered_map>
// Disponibiliza o conjunto de ids visitados na cascata de remoção.
#include <unordered_set>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {

class Database;
template <typename T>
class Query;
template <typename T>
class ProjectedQuery;
template <typename Out>
class MappedQuery;

// Fase até onde o commit avança. `full` é o commit de produção; as demais são
// costuras de teste que congelam o WAL num ponto específico, para a matriz de
// failpoints (Fase 5) simular uma queda em cada janela crítica.
enum class CommitPhase {
    full,                     // WAL(imagens+commit) → aplica → remove o WAL
    stop_after_images,        // só imagens no WAL (queda antes do registro de commit)
    stop_after_commit_record, // WAL commit durável, sem aplicar (queda pós-commit)
    stop_before_wal_cleanup,  // páginas aplicadas, WAL mantido (queda antes do cleanup)
};

// Transação de escrita (Fase 5): RAII sobre o buffer de páginas do PageFile e o
// WAL. Guarda apenas o DatabaseId e resolve o Database pelo registro (como o
// Handle), então sobrevive a movimentos do Database. Uma transação que sai de
// escopo sem commit é revertida no destrutor.
class Transaction {
public:
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&& other) noexcept
        : database_{other.database_}, active_{other.active_}, committed_{other.committed_} {
        other.active_ = false;
        other.committed_ = false;
    }
    Transaction& operator=(Transaction&&) = delete;
    ~Transaction();

    [[nodiscard]] DatabaseId database() const noexcept { return database_; }
    // Grava as páginas no WAL, aplica-as ao arquivo de dados e as torna duráveis.
    [[nodiscard]] Result<void> commit();
    // Costura de teste: interrompe o commit numa fase específica (failpoints).
    [[nodiscard]] Result<void> commit(CommitPhase phase);
    // Descarta as escritas pendentes; nada foi aplicado ao arquivo de dados.
    [[nodiscard]] Result<void> rollback();

private:
    friend class Database;
    explicit Transaction(DatabaseId database) noexcept : database_{database}, active_{true} {}
    DatabaseId database_;
    bool active_{false};
    // Verdadeiro quando o commit foi sincronizado no WAL; não há rollback possível.
    bool committed_{false};
};

// Snapshot de leitura (Fase 6B): fixa a época em que os `get`/`scan`
// versionados enxergam o banco, imune a commits posteriores. RAII — enquanto
// existir, mantém no registro do `Database` a época que segura a versão
// `previous` de qualquer objeto alterado nesse meio-tempo (ver
// ADR-009/PROTOCOLO_FASES.md §Fase 6B). Como o `Handle`/`Transaction`, guarda
// só o `DatabaseId` e resolve o `Database` pelo registro, então sobrevive a
// movimentos do `Database`.
class Snapshot {
public:
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&& other) noexcept
        : database_{other.database_}, epoch_{other.epoch_}, registered_{other.registered_} {
        other.registered_ = false;
    }
    Snapshot& operator=(Snapshot&&) = delete;
    ~Snapshot();

    [[nodiscard]] DatabaseId database() const noexcept { return database_; }
    // Época capturada: `get`/`scan` através deste snapshot enxergam o estado
    // lógico exatamente como estava no último commit até esta época.
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

private:
    friend class Database;
    Snapshot(DatabaseId database, std::uint64_t epoch) noexcept
        : database_{database}, epoch_{epoch}, registered_{true} {}
    DatabaseId database_;
    std::uint64_t epoch_;
    bool registered_{false};
};

// Consulta em streaming (Fases 7A–7D): construtor fluente que descreve um
// scan preguiçoso sobre o tipo T — filtrável (`where`), limitável (`limit`),
// cancelável (`cancel_on`), indexável (`equals`/`between`), projetável
// (`select`/`compute`/`map`) e ordenável/agregável (`order_by`/`top_k`/
// `distinct_by`) — e o materializa em `stream()`. O `Snapshot` é aberto na
// construção e MOVIDO para dentro do Generator em `stream()`. Move-only; os
// métodos fluentes são `&&`-qualificados para encadear sobre a temporária.
template <typename T>
class Query {
public:
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    Query(Query&&) = default;
    Query& operator=(Query&&) = delete;

    // Operador Predicate: mantém só os valores materializados que satisfazem.
    Query&& where(std::function<bool(const T&)> predicate) && {
        predicate_ = std::move(predicate);
        return std::move(*this);
    }
    // Operador Limit: encerra o upstream ao atingir a contagem. 0 = sem limite.
    // Com order_by/top_k/distinct, o limite aplica-se DEPOIS do operador
    // bloqueante (não há pushdown para o scan).
    Query&& limit(std::size_t count) && {
        limit_ = count;
        return std::move(*this);
    }
    // Cancelamento cooperativo: o fluxo encerra quando o token é sinalizado.
    Query&& cancel_on(query::CancellationToken token) && {
        token_ = std::move(token);
        has_token_ = true;
        return std::move(*this);
    }
    // Index Scan por igualdade (Fase 7B).
    Query&& equals(FieldId field, AttributeValue value) && {
        index_field_ = field.value;
        index_lo_ = value;
        index_hi_ = std::move(value);
        return std::move(*this);
    }
    // Index Scan por faixa [lo, hi] inclusive (Fase 7B).
    Query&& between(FieldId field, AttributeValue lo, AttributeValue hi) && {
        index_field_ = field.value;
        index_lo_ = std::move(lo);
        index_hi_ = std::move(hi);
        return std::move(*this);
    }
    // Sort global (Fase 7D, bloqueante). `less(a,b)` define ordem crescente.
    Query&& order_by(std::function<bool(const T&, const T&)> less) && {
        order_less_ = std::move(less);
        return std::move(*this);
    }
    // Top-K (Fase 7D, parcialmente bloqueante): os k "maiores" segundo `less`.
    Query&& top_k(std::size_t k, std::function<bool(const T&, const T&)> less) && {
        top_k_ = k;
        order_less_ = std::move(less);
        return std::move(*this);
    }
    // Distinct por chave (Fase 7D, bloqueante): primeira ocorrência de cada chave.
    Query&& distinct_by(std::function<std::string(const T&)> key) && {
        distinct_key_ = std::move(key);
        return std::move(*this);
    }
    // Instrumentação de pico de memória dos operadores bloqueantes/parcialmente
    // bloqueantes (sort/top_k/distinct). Usado pelos testes de aceite 7D.
    Query&& track_peak(std::size_t* peak) && {
        peak_ = peak;
        return std::move(*this);
    }
    // Projection (Fase 7C): só os FieldIds pedidos, como ProjectedRow.
    [[nodiscard]] ProjectedQuery<T> select(std::vector<FieldId> fields) &&;
    // Computed Function registrada (Fase 7C).
    [[nodiscard]] ProjectedQuery<T> compute(std::string name) &&;
    // Transformação tipada elemento a elemento (Fase 7C).
    template <typename Out>
    [[nodiscard]] MappedQuery<Out> map(std::function<Out(const T&)> fn) &&;
    template <typename Out>
    [[nodiscard]] MappedQuery<Out> map(std::function<Result<Out>(const T&)> fn) &&;

    // Aggregate (Fase 7D, bloqueante): reduz o fluxo a um único Acc.
    template <typename Acc, typename Fold>
    [[nodiscard]] query::Generator<Result<Acc>> aggregate(Acc initial, Fold fold) &&;

    // Constrói o Generator preguiçoso; consome a Query (move o Snapshot).
    [[nodiscard]] query::Generator<Result<T>> stream() &&;

    // Natureza do plano após os operadores enfileirados (Fase 7D).
    [[nodiscard]] query::OperatorNature nature() const noexcept {
        if (top_k_ > 0) {
            return query::OperatorNature::partially_blocking;
        }
        if (order_less_ || distinct_key_) {
            return query::OperatorNature::blocking;
        }
        return query::OperatorNature::streaming;
    }

private:
    friend class Database;
    template <typename U>
    friend class ProjectedQuery;
    Query(Database* database, std::optional<Snapshot> snapshot,
          std::optional<Error> setup_error)
        : database_{database},
          snapshot_{std::move(snapshot)},
          setup_error_{std::move(setup_error)} {}

    Database* database_;
    std::optional<Snapshot> snapshot_;
    std::optional<Error> setup_error_;
    std::function<bool(const T&)> predicate_;
    std::size_t limit_{0};
    query::CancellationToken token_;
    bool has_token_{false};
    std::optional<std::uint16_t> index_field_;
    AttributeValue index_lo_;
    AttributeValue index_hi_;
    std::function<bool(const T&, const T&)> order_less_;
    std::size_t top_k_{0};
    std::function<std::string(const T&)> distinct_key_;
    std::size_t* peak_{nullptr};
};

// Consulta projetada (Fase 7C): produz ProjectedRow com campos selecionados e/ou
// funções computadas registradas. Continua preguiçosa e com memória O(1).
template <typename T>
class ProjectedQuery {
public:
    ProjectedQuery(const ProjectedQuery&) = delete;
    ProjectedQuery& operator=(const ProjectedQuery&) = delete;
    ProjectedQuery(ProjectedQuery&&) = default;
    ProjectedQuery& operator=(ProjectedQuery&&) = delete;

    ProjectedQuery&& select(std::vector<FieldId> fields) && {
        selected_ = std::move(fields);
        return std::move(*this);
    }
    ProjectedQuery&& compute(std::string name) && {
        computed_.push_back(std::move(name));
        return std::move(*this);
    }
    ProjectedQuery&& limit(std::size_t count) && {
        limit_ = count;
        return std::move(*this);
    }

    [[nodiscard]] query::Generator<Result<query::ProjectedRow>> stream() &&;

private:
    friend class Query<T>;
    ProjectedQuery(Query<T> base, std::vector<FieldId> selected, std::vector<std::string> computed)
        : database_{base.database_},
          snapshot_{std::move(base.snapshot_)},
          setup_error_{std::move(base.setup_error_)},
          predicate_{std::move(base.predicate_)},
          limit_{base.limit_},
          token_{std::move(base.token_)},
          has_token_{base.has_token_},
          index_field_{base.index_field_},
          index_lo_{std::move(base.index_lo_)},
          index_hi_{std::move(base.index_hi_)},
          selected_{std::move(selected)},
          computed_{std::move(computed)} {}

    Database* database_;
    std::optional<Snapshot> snapshot_;
    std::optional<Error> setup_error_;
    std::function<bool(const T&)> predicate_;
    std::size_t limit_{0};
    query::CancellationToken token_;
    bool has_token_{false};
    std::optional<std::uint16_t> index_field_;
    AttributeValue index_lo_;
    AttributeValue index_hi_;
    std::vector<FieldId> selected_;
    std::vector<std::string> computed_;
};

// Resultado tipado de `.map(...)` (Fase 7C).
template <typename Out>
class MappedQuery {
public:
    explicit MappedQuery(query::Generator<Result<Out>> stream) : stream_{std::move(stream)} {}
    MappedQuery(const MappedQuery&) = delete;
    MappedQuery& operator=(const MappedQuery&) = delete;
    MappedQuery(MappedQuery&&) = default;
    MappedQuery& operator=(MappedQuery&&) = delete;

    [[nodiscard]] query::Generator<Result<Out>> stream() && { return std::move(stream_); }

private:
    query::Generator<Result<Out>> stream_;
};

// Registro de processo que mantém bancos vivos e permite que Handles resolvam
// seu DatabaseId sem carregar ponteiros.
class DatabaseRegistry {
public:
    [[nodiscard]] Result<DatabaseId> attach(std::shared_ptr<Database> database);
    [[nodiscard]] Result<std::shared_ptr<Database>> find(DatabaseId id) const;
    void detach(DatabaseId id);
    [[nodiscard]] static DatabaseRegistry& instance();

private:
    static std::mutex mutex_;
    static std::unordered_map<std::uint32_t, std::shared_ptr<Database>> databases_;
    static std::uint32_t next_id_;
};

// Chave estável e única por tipo C++, sem depender de RTTI/typeid (que, com
// -static-libstdc++ no MinGW, colide na definição de type_info::operator==).
// Cada instanciação tem seu próprio `anchor`, cujo endereço identifica o tipo.
template <typename T>
const void* type_key() {
    static const char anchor = 0;
    return &anchor;
}

// Banco Orientado a Objetos com API tipada: liga classes C++ ao armazenamento
// persistente (Binding), cria e materializa objetos reais e devolve Handles.
//
// É a fachada do MVP OO embedded. O PageFile é mantido por unique_ptr para ter
// endereço estável — o ObjectStore guarda um PageFile* e o Database precisa
// permanecer movível sem invalidar esse ponteiro.
class Database {
public:
    using Migration = std::function<Result<FieldValues>(const DecodedObject&)>;

    // Cria um banco novo; falha se o arquivo já existir.
    [[nodiscard]] static Result<Database> create(const std::filesystem::path& path);
    // Abre um banco OO existente.
    [[nodiscard]] static Result<Database> open(const std::filesystem::path& path);

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = default;
    Database& operator=(Database&&) = delete;
    ~Database() = default;

    // Registra o binding do tipo T e reconcilia com o catálogo persistido:
    // tipo inexistente → grava; idêntico → adota o id existente; divergente →
    // grava uma nova TypeDefinition e uma nova Baseline.
    template <typename T>
    [[nodiscard]] Result<void> bind(BindingBuilder<T> builder) {
        if (auto usable = check_usable(); !usable) {
            return std::unexpected(usable.error());
        }
        // Binding é configuração de catálogo: não participa da transação do
        // chamador, pois o estado em memória só é atualizado após persistência
        // confirmada pela transação interna.
        if (file_->in_transaction()) {
            return std::unexpected(Error{ErrorCode::transaction_active,
                                         "cannot bind a type while a transaction is active"});
        }
        if (bound_.contains(type_key<T>())) {
            return std::unexpected(
                Error{ErrorCode::binding_mismatch, "C++ type is already bound in this database"});
        }
        auto binding = builder.build();
        if (!binding) {
            return std::unexpected(binding.error());
        }
        // O catálogo é gravado sob uma transação interna (WAL): registrar um tipo
        // novo/evoluído é atômico e sobrevive a uma queda como qualquer escrita.
        auto type_id = persist_binding(*binding);
        if (!type_id) {
            return std::unexpected(type_id.error());
        }
        bound_.insert_or_assign(type_key<T>(), BoundType{std::move(*binding), *type_id});
        return {};
    }

    // Persiste um objeto C++ e devolve seu Handle. Exige uma transação ativa; o
    // tipo precisa estar bound.
    template <typename T>
    [[nodiscard]] Result<Handle<T>> create(Transaction& tx, const T& value) {
        if (auto writable = check_writable(tx); !writable) {
            return std::unexpected(writable.error());
        }
        const BoundType* bound = bound_for(type_key<T>());
        if (bound == nullptr) {
            return std::unexpected(Error{ErrorCode::type_not_found, "type is not bound"});
        }
        auto type = store_.find_type(bound->type_id);
        if (!type) {
            return std::unexpected(type.error());
        }
        auto fields = bound->binding.to_field_values(&value);
        if (!fields) {
            return std::unexpected(fields.error());
        }
        auto id = store_.create_object(type->get(), std::move(*fields));
        if (!id) {
            return std::unexpected(id.error());
        }
        return Handle<T>{database_id_, *id};
    }

    // Devolve um Handle para um objeto existente (verifica a existência).
    template <typename T>
    [[nodiscard]] Result<Handle<T>> get(ObjectId id) {
        if (auto usable = check_usable(); !usable) {
            return std::unexpected(usable.error());
        }
        if (database_id_.value == 0) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "database must be attached before use"});
        }
        const BoundType* bound = bound_for(type_key<T>());
        if (bound == nullptr) {
            return std::unexpected(Error{ErrorCode::type_not_found, "type is not bound"});
        }
        auto object = store_.get(id);
        if (!object) {
            return std::unexpected(object.error());
        }
        auto stored_type = store_.find_type(object->type);
        if (!stored_type) {
            return std::unexpected(stored_type.error());
        }
        if (stored_type->get().name() != bound->binding.type_name()) {
            return std::unexpected(
                Error{ErrorCode::type_mismatch, "object belongs to a different bound type"});
        }
        return Handle<T>{database_id_, id};
    }

    // Reconstrói o objeto C++ a partir do que está persistido. T precisa ser
    // default-constructible (o materializador preenche cada membro bound).
    template <typename T>
    [[nodiscard]] Result<T> materialize(const Handle<T>& handle) {
        if (auto usable = check_usable(); !usable) {
            return std::unexpected(usable.error());
        }
        if (database_id_.value == 0 || handle.database() != database_id_) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "handle belongs to a different database"});
        }
        const BoundType* bound = bound_for(type_key<T>());
        if (bound == nullptr) {
            return std::unexpected(Error{ErrorCode::type_not_found, "type is not bound"});
        }
        auto object = store_.get(handle.id());
        if (!object) {
            return std::unexpected(object.error());
        }
        return materialize_decoded<T>(*bound, *object);
    }

    // Recupera o objeto materializado tal como era visível na época do
    // snapshot (Fase 6B): leitura estável, imune a commits feitos depois que
    // o snapshot foi aberto. Ao contrário de `get`/`materialize`, devolve o
    // valor diretamente — uma leitura por época é um instantâneo, não uma
    // identidade reaproveitável como `Handle`.
    template <typename T>
    [[nodiscard]] Result<T> get(ObjectId id, const Snapshot& snapshot) {
        if (auto usable = check_usable(); !usable) {
            return std::unexpected(usable.error());
        }
        if (snapshot.database() != database_id_) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "snapshot belongs to a different database"});
        }
        const BoundType* bound = bound_for(type_key<T>());
        if (bound == nullptr) {
            return std::unexpected(Error{ErrorCode::type_not_found, "type is not bound"});
        }
        auto object = store_.get_at(id, snapshot.epoch());
        if (!object) {
            return std::unexpected(object.error());
        }
        auto stored_type = store_.find_type(object->type);
        if (!stored_type) {
            return std::unexpected(stored_type.error());
        }
        if (stored_type->get().name() != bound->binding.type_name()) {
            return std::unexpected(
                Error{ErrorCode::type_mismatch, "object belongs to a different bound type"});
        }
        return materialize_decoded<T>(*bound, *object);
    }

    // Visita, na ordem física, cada objeto do tipo T visível na época do
    // snapshot — a versão "leitura estável" de scan (Fase 6B). Objetos de
    // outros tipos são ignorados; a materialização usa o mesmo ProjectionPlan
    // cacheado de `get`/`materialize`.
    template <typename T>
    [[nodiscard]] Result<void> scan(const Snapshot& snapshot,
                                    const std::function<Result<void>(const T&)>& visitor) {
        if (auto usable = check_usable(); !usable) {
            return std::unexpected(usable.error());
        }
        if (snapshot.database() != database_id_) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "snapshot belongs to a different database"});
        }
        const BoundType* bound = bound_for(type_key<T>());
        if (bound == nullptr) {
            return std::unexpected(Error{ErrorCode::type_not_found, "type is not bound"});
        }
        return store_.scan_at(
            snapshot.epoch(), [&](const DecodedObject& object) -> Result<void> {
                auto stored_type = store_.find_type(object.type);
                if (!stored_type || stored_type->get().name() != bound->binding.type_name()) {
                    return {};
                }
                auto value = materialize_decoded<T>(*bound, object);
                if (!value) {
                    return std::unexpected(value.error());
                }
                return visitor(*value);
            });
    }

    // Regrava um objeto usando a definição corrente (migração preguiçosa).
    // Exige uma transação ativa.
    template <typename T>
    [[nodiscard]] Result<void> update(Transaction& tx, const Handle<T>& handle, const T& value) {
        if (auto writable = check_writable(tx); !writable) {
            return std::unexpected(writable.error());
        }
        if (handle.database() != database_id_) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "handle belongs to a different database"});
        }
        const BoundType* bound = bound_for(type_key<T>());
        if (bound == nullptr) {
            return std::unexpected(Error{ErrorCode::type_not_found, "type is not bound"});
        }
        auto type = store_.find_type(bound->type_id);
        if (!type) {
            return std::unexpected(type.error());
        }
        auto stored = store_.get(handle.id());
        if (!stored) {
            return std::unexpected(stored.error());
        }
        auto stored_type = store_.find_type(stored->type);
        if (!stored_type) {
            return std::unexpected(stored_type.error());
        }
        if (stored_type->get().name() != bound->binding.type_name()) {
            return std::unexpected(
                Error{ErrorCode::type_mismatch, "object belongs to a different bound type"});
        }
        auto fields = bound->binding.to_field_values(&value);
        if (!fields) {
            return std::unexpected(fields.error());
        }
        return store_.update(handle.id(), type->get(), std::move(*fields),
                            oldest_open_snapshot_epoch());
    }

    // Abre uma consulta em streaming sobre o tipo T (Fase 7A). Abre um snapshot
    // na época corrente e devolve um construtor fluente; o consumo real
    // (preguiçoso) começa em `.stream()`. Ex.:
    //   for (auto& r : db.query<Account>().where(...).limit(10).stream()) { ... }
    template <typename T>
    [[nodiscard]] Query<T> query() {
        const BoundType* bound = bound_for(type_key<T>());
        if (bound == nullptr) {
            return Query<T>{this, std::nullopt,
                            Error{ErrorCode::type_not_found, "type is not bound"}};
        }
        auto opened = snapshot();
        if (!opened) {
            return Query<T>{this, std::nullopt, opened.error()};
        }
        return Query<T>{this, std::move(*opened), std::nullopt};
    }

    // Cria um índice B+ tree sobre o campo `field` do tipo T (Fase 7B): monta a
    // árvore, indexa os objetos atuais (backfill) e registra no catálogo, tudo
    // numa transação interna (WAL). Depois, `query<T>().equals/.between` usam a
    // árvore. Falha se houver transação ativa ou se o tipo não estiver bound.
    template <typename T>
    [[nodiscard]] Result<void> create_index(FieldId field) {
        if (auto usable = check_usable(); !usable) {
            return std::unexpected(usable.error());
        }
        if (database_id_.value == 0) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "database must be attached before use"});
        }
        if (file_->in_transaction()) {
            return std::unexpected(Error{ErrorCode::transaction_active,
                                         "cannot create an index inside a transaction"});
        }
        const BoundType* bound = bound_for(type_key<T>());
        if (bound == nullptr) {
            return std::unexpected(Error{ErrorCode::type_not_found, "type is not bound"});
        }
        auto type = store_.find_type(bound->type_id);
        if (!type) {
            return std::unexpected(type.error());
        }
        current_tx_id_ = next_tx_id_++;
        commit_durable_ = false;
        file_->begin_transaction();
        if (auto created = store_.create_index(type->get(), field); !created) {
            (void)rollback_transaction();
            return std::unexpected(created.error());
        }
        if (auto committed = commit_transaction(CommitPhase::full); !committed) {
            if (commit_is_durable()) {
                require_recovery();
                return std::unexpected(Error{ErrorCode::commit_recovery_required,
                                             "index commit is durable but requires recovery: " +
                                                 committed.error().message});
            }
            (void)rollback_transaction();
            return std::unexpected(committed.error());
        }
        return {};
    }

    // Registra uma Computed Function nomeada sobre o tipo T (Fase 7C). Avaliada
    // elemento a elemento em `.compute("nome")`. Substitui registro anterior
    // com o mesmo nome para o mesmo tipo.
    template <typename T>
    Result<void> register_computed(std::string name,
                                   std::function<Result<AttributeValue>(const T&)> function) {
        if (name.empty()) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "computed function name must not be empty"});
        }
        if (!function) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "computed function must not be empty"});
        }
        computed_fns_[type_key<T>()][std::move(name)] =
            [fn = std::move(function)](const void* object) -> Result<AttributeValue> {
                return fn(*static_cast<const T*>(object));
            };
        return {};
    }

    Result<void> register_migration(std::string type_name, std::uint64_t from_type_id,
                                    Migration migration);

    // Inicia uma transação de escrita. Falha se já houver uma em andamento
    // (single-writer) ou se o banco não estiver anexado ao registro.
    [[nodiscard]] Result<Transaction> begin();

    // Abre um snapshot fixado na época corrente (Fase 6B): leituras via
    // `get(id, snapshot)`/`scan(snapshot, ...)` continuam vendo exatamente
    // este estado lógico mesmo que outras transações commitem depois.
    [[nodiscard]] Result<Snapshot> snapshot() {
        if (auto usable = check_usable(); !usable) {
            return std::unexpected(usable.error());
        }
        if (database_id_.value == 0) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "database must be attached before use"});
        }
        const auto current_epoch = store_.epoch();
        register_snapshot_epoch(current_epoch);
        return Snapshot{database_id_, current_epoch};
    }

    [[nodiscard]] Result<TypeDefinitionId> define_type(Transaction& tx,
                                                        TypeDefinition definition);

    // Executa `fn(Transaction&)` sob uma transação: término normal com Ok →
    // commit; retorno de erro → rollback; exceção → rollback (via RAII do
    // destrutor da Transaction, que reverte quando não houve commit). É o
    // contrato reutilizado pela Fase 9 (Operations).
    template <typename Fn>
    [[nodiscard]] auto transact(Fn&& fn) -> decltype(fn(std::declval<Transaction&>())) {
        using ResultType = decltype(fn(std::declval<Transaction&>()));
        auto transaction = begin();
        if (!transaction) {
            return ResultType{std::unexpected(transaction.error())};
        }
        auto outcome = fn(*transaction);
        if (!outcome) {
            (void)transaction->rollback();
            return outcome;
        }
        if (auto committed = transaction->commit(); !committed) {
            return ResultType{std::unexpected(committed.error())};
        }
        return outcome;
    }

    // Substitui a fábrica do arquivo do WAL. Uso restrito a testes: injeta um
    // FailpointFile para simular falhas de I/O reais no commit (Fase 5).
    void set_wal_file_factory(tx::WalFileFactory factory) {
        wal_factory_ = std::move(factory);
    }

    // Uso restrito a testes: faz a aplicação das páginas falhar após N páginas,
    // simulando uma queda no meio do apply (Fase 5, failpoints).
    void set_apply_failpoint(std::size_t pages_before_failure) noexcept {
        file_->set_apply_failpoint(pages_before_failure);
    }

    [[nodiscard]] Result<TypeDefinitionId> object_type(ObjectId id) {
        if (auto usable = check_usable(); !usable) {
            return std::unexpected(usable.error());
        }
        auto object = store_.get(id);
        if (!object) {
            return std::unexpected(object.error());
        }
        return object->type;
    }

    [[nodiscard]] const std::optional<Baseline>& current_baseline() const noexcept {
        return store_.current_baseline();
    }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return store_.epoch(); }
    [[nodiscard]] Result<std::reference_wrapper<const Baseline>> find_baseline(
        BaselineId id) const {
        return store_.find_baseline(id);
    }
    // Total de registros físicos vivos no heap de dados (Fase 6C): current +
    // versões `previous` preservadas + órfãos ainda não coletados. Diagnóstico
    // para observar o efeito do GC.
    [[nodiscard]] std::uint64_t data_record_count() const noexcept {
        return store_.data_record_count();
    }

    // Estado de versionamento de um objeto (Fase 6): época/localização da
    // versão `current` e, se retida, da `previous`. Diagnóstico read-only.
    [[nodiscard]] Result<IdentityMap::VersionInfo> version_info(ObjectId id) {
        if (auto usable = check_usable(); !usable) {
            return std::unexpected(usable.error());
        }
        return store_.version_info(id);
    }

    // Páginas de dados lidas pelo scan preguiçoso desde o último reset
    // (instrumentação do critério TTFR da Fase 7A: `limit 1` deve manter isto
    // em ≤ 2). Diagnóstico de sessão, zerável.
    [[nodiscard]] std::uint64_t data_pages_read() const noexcept {
        return store_.data_pages_read();
    }
    void reset_data_pages_read() noexcept { store_.reset_data_pages_read(); }

    // Recicla o espaço das versões antigas que nenhum snapshot aberto ainda
    // pode enxergar (Fase 6C). Roda numa transação própria (as liberações
    // passam pelo WAL); respeita o snapshot aberto mais antigo, então versões
    // ainda visíveis são preservadas. Falha se houver transação ativa
    // (single-writer). Devolve quantos registros físicos foram recuperados.
    [[nodiscard]] Result<std::size_t> collect_garbage();

    // Remove um objeto pelo id (o id nunca é reutilizado). Exige uma transação
    // ativa. Referências de composição (OwnedRef) são removidas em cascata, em
    // profundidade-primeiro; um ciclo de posse é detectado e falha com
    // invalid_argument (ADR-008). Referências simples (Ref) não são seguidas: o
    // alvo some e a resolução posterior falha com record_not_found.
    [[nodiscard]] Result<void> remove(Transaction& tx, ObjectId id);

    // Persiste no dispositivo tudo que foi escrito fora de transação. Dentro de
    // uma transação, a durabilidade vem do commit; este flush é para o catálogo
    // gravado por bind() (que não passa por transação nesta fase).
    [[nodiscard]] Result<void> flush() {
        if (auto usable = check_usable(); !usable) {
            return std::unexpected(usable.error());
        }
        return file_->flush();
    }

    // Devolve um BlobStore sobre o mesmo arquivo, base das coleções e binários
    // grandes. É leve (só referencia o PageFile) e pode ser criado sob demanda.
    [[nodiscard]] BlobStore blobs() noexcept { return BlobStore{*file_, database_id_, true}; }

private:
    // Um tipo C++ ligado ao seu binding e ao id de tipo persistido.
    struct BoundType {
        Binding binding;
        TypeDefinitionId type_id;
        mutable std::unordered_map<std::uint64_t, ProjectionPlan> plans;
    };

    Database(std::unique_ptr<storage::PageFile> file, ObjectStore store,
             std::filesystem::path wal_path)
        : file_{std::move(file)}, store_{std::move(store)}, wal_path_{std::move(wal_path)} {}

    // Confere que uma escrita pode prosseguir: banco anexado, transação da mesma
    // instância e de fato ativa no PageFile.
    [[nodiscard]] Result<void> check_writable(const Transaction& tx) const {
        if (auto usable = check_usable(); !usable) {
            return std::unexpected(usable.error());
        }
        if (database_id_.value == 0) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "database must be attached before use"});
        }
        if (tx.database() != database_id_) {
            return std::unexpected(Error{ErrorCode::invalid_argument,
                                         "transaction belongs to a different database"});
        }
        if (!file_->in_transaction()) {
            return std::unexpected(
                Error{ErrorCode::transaction_required, "no active transaction"});
        }
        return {};
    }

    // Depois de uma falha pós-commit, o WAL é a fonte de verdade. Esta instância
    // pode ter páginas e metadados em memória parcialmente aplicados, portanto
    // só uma reabertura (que roda recovery) volta a expor o banco com segurança.
    [[nodiscard]] Result<void> check_usable() const {
        if (recovery_required_) {
            return std::unexpected(Error{ErrorCode::database_recovery_required,
                                         "database must be reopened after a durable commit failure"});
        }
        return {};
    }

    [[nodiscard]] bool commit_is_durable() const noexcept { return commit_durable_; }
    void require_recovery() noexcept { recovery_required_ = true; }

    // Grava o buffer da transação no WAL, aplica-o e (em full) remove o WAL.
    [[nodiscard]] Result<void> commit_transaction(CommitPhase phase);
    // Descarta o buffer da transação e remove qualquer WAL residual.
    [[nodiscard]] Result<void> rollback_transaction();
    // Reconstrói `store_` a partir do que está atualmente no arquivo. Chamado
    // após o descarte de uma transação: TableHeap/IdentityMap/CatalogStore
    // guardam contadores em memória (record_count, page_count, cadeia de
    // páginas) atualizados otimisticamente durante a transação; só o buffer de
    // páginas do PageFile é revertido no rollback, então sem isto a próxima
    // escrita real usaria contadores que nunca existiram em disco, corrompendo
    // a raiz do heap. `ObjectStore::open` é puramente leitura (sem alocar
    // páginas), então é seguro chamá-lo de novo a qualquer momento.
    [[nodiscard]] Result<void> resync_store_after_rollback();
    friend class Transaction;

    // Remove um objeto e, recursivamente, os filhos referenciados por OwnedRef.
    // `visited` guarda os ids em andamento para detectar ciclos de posse.
    [[nodiscard]] Result<void> remove_cascade(ObjectId id,
                                              std::unordered_set<std::uint64_t>& visited);

    // Registra o tipo do binding ou adota o já persistido (validando estrutura).
    [[nodiscard]] Result<TypeDefinitionId> register_or_adopt(const Binding& binding);
    // Persiste o binding sob uma transação interna (ou na transação já ativa do
    // chamador), tornando as escritas de catálogo atômicas e registradas no WAL.
    [[nodiscard]] Result<TypeDefinitionId> persist_binding(const Binding& binding);
    // Localiza o BoundType de um tipo C++, ou nullptr se não estiver bound.
    [[nodiscard]] const BoundType* bound_for(const void* type) const;
    [[nodiscard]] const Migration* migration_for(std::string_view type_name,
                                                 TypeDefinitionId from_type_id) const;

    // Materializa um DecodedObject já resolvido (current ou de um snapshot) em
    // T, compartilhado por `materialize`, `get(id, Snapshot)` e
    // `scan(Snapshot, ...)` — a única diferença entre essas três leituras é de
    // onde o DecodedObject veio, nunca como ele vira T.
    template <typename T>
    [[nodiscard]] Result<T> materialize_decoded(const BoundType& bound,
                                                const DecodedObject& object) {
        T result{};
        const Migration* migration = migration_for(bound.binding.type_name(), object.type);
        if (migration != nullptr) {
            auto migrated = (*migration)(object);
            if (!migrated) {
                return std::unexpected(migrated.error());
            }
            if (auto materialized = bound.binding.materialize(*migrated, &result);
                !materialized) {
                return std::unexpected(materialized.error());
            }
            return result;
        }
        auto plan = bound.plans.find(object.type.value);
        if (plan == bound.plans.end()) {
            auto stored_type = store_.find_type(object.type);
            if (!stored_type) {
                return std::unexpected(stored_type.error());
            }
            auto built = ProjectionPlan::build(stored_type->get(), bound.binding);
            if (!built) {
                return std::unexpected(built.error());
            }
            plan = bound.plans.emplace(object.type.value, std::move(*built)).first;
        }
        if (auto materialized = plan->second.materialize(object, bound.binding, &result);
            !materialized) {
            return std::unexpected(materialized.error());
        }
        return result;
    }

    // Fonte typed do pipeline de streaming (Fase 7A): possui o `Snapshot` (por
    // valor → vive na moldura da coroutine, logo o fluxo mantém o estado lógico
    // estável por toda a leitura), percorre o scan preguiçoso já filtrado pelo
    // tipo, materializa cada objeto em T e aplica Predicate e Limit. `Limit`
    // encerra (`co_return`) destruindo o scan a montante — o curto-circuito.
    template <typename T>
    query::Generator<Result<T>> run_stream(Snapshot snapshot,
                                           std::function<bool(const T&)> predicate,
                                           std::size_t limit_count, query::CancellationToken token,
                                           bool has_token) {
        std::size_t emitted = 0;
        // Fonte percorre TODOS os objetos visíveis na época; o filtro por tipo é
        // por NOME (todas as versões de schema do mesmo tipo lógico), igual a
        // Database::scan<T> — um objeto de uma versão antiga ainda é um T.
        for (auto& item : store_.scan_stream(snapshot.epoch(), std::nullopt)) {
            if (has_token && token.cancelled()) {
                co_return;
            }
            if (!item) {
                co_yield Result<T>{std::unexpected(item.error())};
                co_return;
            }
            const BoundType* bound = bound_for(type_key<T>());
            if (bound == nullptr) {
                co_yield Result<T>{std::unexpected(Error{ErrorCode::type_not_found,
                                                         "type is not bound"})};
                co_return;
            }
            auto stored_type = store_.find_type(item->type);
            if (!stored_type || stored_type->get().name() != bound->binding.type_name()) {
                continue;  // objeto de outro tipo lógico
            }
            auto value = materialize_decoded<T>(*bound, *item);
            if (!value) {
                co_yield Result<T>{std::unexpected(value.error())};
                co_return;
            }
            if (predicate && !predicate(*value)) {
                continue;
            }
            co_yield Result<T>{std::move(*value)};
            if (limit_count != 0 && ++emitted >= limit_count) {
                co_return;
            }
        }
    }

    // Index Scan (Fase 7B): a B+ tree do campo gera os candidatos (em ordem de
    // valor); cada um é revalidado contra a versão do snapshot antes de
    // materializar, aplicar Predicate e Limit. O Snapshot vive na moldura da
    // coroutine (leitura estável). Sem índice sobre o campo → erro.
    template <typename T>
    query::Generator<Result<T>> run_index_stream(Snapshot snapshot, std::uint16_t field,
                                                 AttributeValue lo, AttributeValue hi,
                                                 std::function<bool(const T&)> predicate,
                                                 std::size_t limit_count,
                                                 query::CancellationToken token, bool has_token) {
        const BoundType* bound = bound_for(type_key<T>());
        if (bound == nullptr) {
            co_yield Result<T>{std::unexpected(Error{ErrorCode::type_not_found,
                                                     "type is not bound"})};
            co_return;
        }
        auto ids = store_.index_range(bound->binding.type_name(), field, lo, hi);
        if (!ids) {
            co_yield Result<T>{std::unexpected(ids.error())};
            co_return;
        }
        std::size_t emitted = 0;
        for (const auto& id : *ids) {
            if (has_token && token.cancelled()) {
                co_return;
            }
            auto candidate = store_.index_candidate_at(id, snapshot.epoch(), field, lo, hi);
            if (!candidate) {
                co_yield Result<T>{std::unexpected(candidate.error())};
                co_return;
            }
            if (!*candidate) {
                continue;  // não visível ou fora da faixa na versão do snapshot
            }
            auto value = materialize_decoded<T>(*bound, **candidate);
            if (!value) {
                co_yield Result<T>{std::unexpected(value.error())};
                co_return;
            }
            if (predicate && !predicate(*value)) {
                continue;
            }
            co_yield Result<T>{std::move(*value)};
            if (limit_count != 0 && ++emitted >= limit_count) {
                co_return;
            }
        }
    }

    // Fluxo que cede um único erro — usado quando a configuração da Query falhou
    // (tipo não bound, snapshot não pôde abrir) antes de qualquer leitura.
    template <typename T>
    query::Generator<Result<T>> failed_stream(Error error) {
        co_yield Result<T>{std::unexpected(std::move(error))};
    }

    // Monta uma ProjectedRow a partir de um objeto T (identidade, campos e
    // computados). FieldId 0 seleciona o ObjectId como o campo sintético `id`.
    template <typename T>
    Result<query::ProjectedRow> project_object(const T& value, ObjectId object_id,
                                               const std::vector<FieldId>& selected,
                                               const std::vector<std::string>& computed) const {
        query::ProjectedRow row;
        const BoundType* bound = bound_for(type_key<T>());
        if (bound == nullptr) {
            return std::unexpected(Error{ErrorCode::type_not_found, "type is not bound"});
        }
        for (const FieldId field : selected) {
            if (field.value == 0) {
                row.fields.push_back(
                    query::ProjectedField{field, "id", AttributeValue{object_id}});
                continue;
            }
            const FieldBinder* binder = nullptr;
            for (const auto& candidate : bound->binding.fields()) {
                if (candidate.id.value == field.value) {
                    binder = &candidate;
                    break;
                }
            }
            if (binder == nullptr) {
                return std::unexpected(
                    Error{ErrorCode::invalid_argument, "projected field is not in the binding"});
            }
            auto loaded = binder->load(&value);
            if (!loaded) {
                return std::unexpected(loaded.error());
            }
            row.fields.push_back(
                query::ProjectedField{binder->id, binder->name, std::move(*loaded)});
        }
        if (!computed.empty()) {
            const auto type_it = computed_fns_.find(type_key<T>());
            for (const auto& name : computed) {
                if (type_it == computed_fns_.end()) {
                    return std::unexpected(
                        Error{ErrorCode::type_not_found, "computed function is not registered"});
                }
                const auto fn_it = type_it->second.find(name);
                if (fn_it == type_it->second.end()) {
                    return std::unexpected(
                        Error{ErrorCode::type_not_found, "computed function is not registered"});
                }
                auto value_attr = fn_it->second(&value);
                if (!value_attr) {
                    return std::unexpected(value_attr.error());
                }
                row.fields.push_back(
                    query::ProjectedField{FieldId{0}, name, std::move(*value_attr)});
            }
        }
        return row;
    }

    // Scan/Index Scan + Projection/Compute (Fase 7C). Preserva o ObjectId do
    // DecodedObject quando o campo sintético `id` é selecionado.
    template <typename T>
    query::Generator<Result<query::ProjectedRow>> run_projected_stream(
        Snapshot snapshot, std::function<bool(const T&)> predicate, std::size_t limit_count,
        query::CancellationToken token, bool has_token, std::optional<std::uint16_t> index_field,
        AttributeValue lo, AttributeValue hi, std::vector<FieldId> selected,
        std::vector<std::string> computed) {
        const BoundType* bound = bound_for(type_key<T>());
        if (bound == nullptr) {
            co_yield Result<query::ProjectedRow>{
                std::unexpected(Error{ErrorCode::type_not_found, "type is not bound"})};
            co_return;
        }

        auto emit = [&](const DecodedObject& decoded,
                        const T& value) -> Result<query::ProjectedRow> {
            return project_object<T>(value, decoded.id, selected, computed);
        };

        std::size_t emitted = 0;
        if (index_field) {
            auto ids = store_.index_range(bound->binding.type_name(), *index_field, lo, hi);
            if (!ids) {
                co_yield Result<query::ProjectedRow>{std::unexpected(ids.error())};
                co_return;
            }
            for (const auto& id : *ids) {
                if (has_token && token.cancelled()) {
                    co_return;
                }
                auto candidate =
                    store_.index_candidate_at(id, snapshot.epoch(), *index_field, lo, hi);
                if (!candidate) {
                    co_yield Result<query::ProjectedRow>{std::unexpected(candidate.error())};
                    co_return;
                }
                if (!*candidate) {
                    continue;
                }
                auto value = materialize_decoded<T>(*bound, **candidate);
                if (!value) {
                    co_yield Result<query::ProjectedRow>{std::unexpected(value.error())};
                    co_return;
                }
                if (predicate && !predicate(*value)) {
                    continue;
                }
                auto row = emit(**candidate, *value);
                if (!row) {
                    co_yield Result<query::ProjectedRow>{std::unexpected(row.error())};
                    co_return;
                }
                co_yield Result<query::ProjectedRow>{std::move(*row)};
                if (limit_count != 0 && ++emitted >= limit_count) {
                    co_return;
                }
            }
            co_return;
        }

        for (auto& item : store_.scan_stream(snapshot.epoch(), std::nullopt)) {
            if (has_token && token.cancelled()) {
                co_return;
            }
            if (!item) {
                co_yield Result<query::ProjectedRow>{std::unexpected(item.error())};
                co_return;
            }
            auto stored_type = store_.find_type(item->type);
            if (!stored_type || stored_type->get().name() != bound->binding.type_name()) {
                continue;
            }
            auto value = materialize_decoded<T>(*bound, *item);
            if (!value) {
                co_yield Result<query::ProjectedRow>{std::unexpected(value.error())};
                co_return;
            }
            if (predicate && !predicate(*value)) {
                continue;
            }
            auto row = emit(*item, *value);
            if (!row) {
                co_yield Result<query::ProjectedRow>{std::unexpected(row.error())};
                co_return;
            }
            co_yield Result<query::ProjectedRow>{std::move(*row)};
            if (limit_count != 0 && ++emitted >= limit_count) {
                co_return;
            }
        }
    }

    template <typename U>
    friend class Query;
    template <typename U>
    friend class ProjectedQuery;

    // Época do snapshot aberto mais antigo (nullopt se nenhum estiver aberto).
    // Consultada antes de toda escrita que possa conflitar com uma versão
    // `previous` ainda visível (Fase 6B) e pelo GC (Fase 6C). O acesso ao
    // registro é serializado pelo `snapshot_registry_mutex_`: abertura/fecho de
    // snapshot (lado leitor) e escrita/GC (lado escritor) sincronizam só neste
    // ponto curto, sem prender a duração das leituras (Fase 6C).
    [[nodiscard]] std::optional<std::uint64_t> oldest_open_snapshot_epoch() const {
        const std::scoped_lock lock{*snapshot_registry_mutex_};
        return open_snapshot_epochs_.empty() ? std::nullopt
                                             : std::optional{*open_snapshot_epochs_.begin()};
    }
    friend class Snapshot;
    void register_snapshot_epoch(std::uint64_t epoch) {
        const std::scoped_lock lock{*snapshot_registry_mutex_};
        open_snapshot_epochs_.insert(epoch);
    }
    void unregister_snapshot_epoch(std::uint64_t epoch) {
        const std::scoped_lock lock{*snapshot_registry_mutex_};
        const auto it = open_snapshot_epochs_.find(epoch);
        if (it != open_snapshot_epochs_.end()) {
            open_snapshot_epochs_.erase(it);
        }
    }

    friend class DatabaseRegistry;
    void set_database_id(DatabaseId id) noexcept { database_id_ = id; }

    std::unique_ptr<storage::PageFile> file_;
    ObjectStore store_;
    std::unordered_map<const void*, BoundType> bound_;
    std::unordered_map<std::string, std::unordered_map<std::uint64_t, Migration>> migrations_;
    // Computed Functions (Fase 7C): type_key → (nome → invocador type-erased).
    std::unordered_map<const void*,
                       std::unordered_map<std::string, std::function<Result<AttributeValue>(const void*)>>>
        computed_fns_;
    DatabaseId database_id_{};
    // Caminho do write-ahead log (`<db>.wal`).
    std::filesystem::path wal_path_;
    // Id da transação corrente e o próximo a atribuir (monotônico por sessão).
    std::uint64_t current_tx_id_{0};
    std::uint64_t next_tx_id_{1};
    // Fábrica do arquivo do WAL; produção usa NativeFile, testes injetam falhas.
    tx::WalFileFactory wal_factory_{tx::open_native_wal_sink};
    // Ativado após o sync do registro commit no WAL. A partir daí rollback não
    // pode apagar o log: qualquer falha posterior exige reabertura e redo.
    bool commit_durable_{false};
    bool recovery_required_{false};
    // Épocas dos snapshots atualmente abertos (multiset: dois snapshots podem
    // capturar a mesma época). O mínimo é a época mais antiga ainda visível,
    // consultada por toda escrita para decidir conflito (Fase 6B) e pelo GC
    // (Fase 6C). Protegido por `snapshot_registry_mutex_`.
    std::multiset<std::uint64_t> open_snapshot_epochs_;
    // Serializa o acesso ao registro de épocas entre o lado leitor (Snapshot
    // RAII) e o lado escritor (escrita/commit/GC). Mantido por unique_ptr para
    // o `Database` continuar movível (std::mutex não é movível) — a fábrica
    // move o `Database` para dentro de um shared_ptr. A serialização de commits
    // em si é dada pela guarda single-writer de `begin()` (Fase 5): uma segunda
    // transação falha com `transaction_active`.
    std::unique_ptr<std::mutex> snapshot_registry_mutex_{std::make_unique<std::mutex>()};
};

template <typename T>
query::Generator<Result<T>> Query<T>::stream() && {
    if (setup_error_) {
        return database_->template failed_stream<T>(std::move(*setup_error_));
    }
    const bool blocking_tail = top_k_ > 0 || static_cast<bool>(order_less_) ||
                               static_cast<bool>(distinct_key_);
    const std::size_t scan_limit = blocking_tail ? 0 : limit_;

    query::Generator<Result<T>> gen = [&]() {
        if (index_field_) {
            return database_->template run_index_stream<T>(
                std::move(*snapshot_), *index_field_, std::move(index_lo_), std::move(index_hi_),
                std::move(predicate_), scan_limit, std::move(token_), has_token_);
        }
        return database_->template run_stream<T>(std::move(*snapshot_), std::move(predicate_),
                                                 scan_limit, std::move(token_), has_token_);
    }();

    // top_k exige order_less_ (definido em Query::top_k). Sem comparador
    // explícito não há fallback std::less<T>: T tipicamente não é ordenável.
    if (top_k_ > 0 && order_less_) {
        gen = query::top_k<T>(std::move(gen), top_k_, std::move(order_less_), peak_);
    } else if (order_less_) {
        gen = query::sort<T>(std::move(gen), std::move(order_less_), peak_);
    }
    if (distinct_key_) {
        gen = query::distinct<T>(std::move(gen), std::move(distinct_key_), peak_);
    }
    if (blocking_tail && limit_ != 0) {
        gen = query::limit<T>(std::move(gen), limit_);
    }
    return gen;
}

template <typename T>
template <typename Acc, typename Fold>
query::Generator<Result<Acc>> Query<T>::aggregate(Acc initial, Fold fold) && {
    return query::aggregate<T, Acc>(std::move(*this).stream(), std::move(initial), std::move(fold));
}

template <typename T>
ProjectedQuery<T> Query<T>::select(std::vector<FieldId> fields) && {
    return ProjectedQuery<T>{std::move(*this), std::move(fields), {}};
}

template <typename T>
ProjectedQuery<T> Query<T>::compute(std::string name) && {
    std::vector<std::string> names;
    names.push_back(std::move(name));
    return ProjectedQuery<T>{std::move(*this), {}, std::move(names)};
}

template <typename T>
template <typename Out>
MappedQuery<Out> Query<T>::map(std::function<Out(const T&)> fn) && {
    auto source = std::move(*this).stream();
    return MappedQuery<Out>{query::project<T, Out>(
        std::move(source), [fn = std::move(fn)](const T& value) -> Result<Out> { return fn(value); })};
}

template <typename T>
template <typename Out>
MappedQuery<Out> Query<T>::map(std::function<Result<Out>(const T&)> fn) && {
    auto source = std::move(*this).stream();
    return MappedQuery<Out>{
        query::compute<T, Out>(std::move(source), [fn = std::move(fn)](const T& value) {
            return fn(value);
        })};
}

template <typename T>
query::Generator<Result<query::ProjectedRow>> ProjectedQuery<T>::stream() && {
    if (setup_error_) {
        return database_->template failed_stream<query::ProjectedRow>(std::move(*setup_error_));
    }
    return database_->template run_projected_stream<T>(
        std::move(*snapshot_), std::move(predicate_), limit_, std::move(token_), has_token_,
        index_field_, std::move(index_lo_), std::move(index_hi_), std::move(selected_),
        std::move(computed_));
}

template <typename T>
template <auto Member>
Result<member_type_t<decltype(Member)>> Handle<T>::get() const {
    auto database = DatabaseRegistry::instance().find(database_);
    if (!database) {
        return std::unexpected(database.error());
    }
    auto object = (*database)->materialize(*this);
    if (!object) {
        return std::unexpected(object.error());
    }
    return (*object).*Member;
}

template <typename T>
template <auto Member, typename V>
Result<void> Handle<T>::set(Transaction& transaction, V&& value) const {
    if (transaction.database() != database_) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "transaction belongs to a different database"});
    }
    auto database = DatabaseRegistry::instance().find(database_);
    if (!database) {
        return std::unexpected(database.error());
    }
    auto object = (*database)->materialize(*this);
    if (!object) {
        return std::unexpected(object.error());
    }
    (*object).*Member = std::forward<V>(value);
    return (*database)->update(transaction, *this, *object);
}

} // namespace modb::object
