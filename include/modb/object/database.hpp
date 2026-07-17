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
// Importa PageFile, mantido por ponteiro estável.
#include "modb/storage/page_file.hpp"
// Importa a fábrica do WAL (injeção de failpoint) usada no commit.
#include "modb/tx/wal.hpp"

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
// Disponibiliza o mapa de bindings.
#include <unordered_map>
// Disponibiliza o conjunto de ids visitados na cascata de remoção.
#include <unordered_set>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {

class Database;

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
        : database_{other.database_}, active_{other.active_} {
        other.active_ = false;
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
        T result{};
        const Migration* migration = migration_for(bound->binding.type_name(), object->type);
        if (migration != nullptr) {
            auto migrated = (*migration)(*object);
            if (!migrated) {
                return std::unexpected(migrated.error());
            }
            if (auto materialized = bound->binding.materialize(*migrated, &result);
                !materialized) {
                return std::unexpected(materialized.error());
            }
            return result;
        }
        auto plan = bound->plans.find(object->type.value);
        if (plan == bound->plans.end()) {
            auto stored_type = store_.find_type(object->type);
            if (!stored_type) {
                return std::unexpected(stored_type.error());
            }
            auto built = ProjectionPlan::build(stored_type->get(), bound->binding);
            if (!built) {
                return std::unexpected(built.error());
            }
            plan = bound->plans.emplace(object->type.value, std::move(*built)).first;
        }
        if (auto materialized = plan->second.materialize(*object, bound->binding, &result);
            !materialized) {
            return std::unexpected(materialized.error());
        }
        return result;
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
        return store_.update(handle.id(), type->get(), std::move(*fields));
    }

    Result<void> register_migration(std::string type_name, std::uint64_t from_type_id,
                                    Migration migration);

    // Inicia uma transação de escrita. Falha se já houver uma em andamento
    // (single-writer) ou se o banco não estiver anexado ao registro.
    [[nodiscard]] Result<Transaction> begin();

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
        auto object = store_.get(id);
        if (!object) {
            return std::unexpected(object.error());
        }
        return object->type;
    }

    [[nodiscard]] const std::optional<Baseline>& current_baseline() const noexcept {
        return store_.current_baseline();
    }
    [[nodiscard]] Result<std::reference_wrapper<const Baseline>> find_baseline(
        BaselineId id) const {
        return store_.find_baseline(id);
    }

    // Remove um objeto pelo id (o id nunca é reutilizado). Exige uma transação
    // ativa. Referências de composição (OwnedRef) são removidas em cascata, em
    // profundidade-primeiro; um ciclo de posse é detectado e falha com
    // invalid_argument (ADR-008). Referências simples (Ref) não são seguidas: o
    // alvo some e a resolução posterior falha com record_not_found.
    [[nodiscard]] Result<void> remove(Transaction& tx, ObjectId id);

    // Persiste no dispositivo tudo que foi escrito fora de transação. Dentro de
    // uma transação, a durabilidade vem do commit; este flush é para o catálogo
    // gravado por bind() (que não passa por transação nesta fase).
    [[nodiscard]] Result<void> flush() { return file_->flush(); }

    // Devolve um BlobStore sobre o mesmo arquivo, base das coleções e binários
    // grandes. É leve (só referencia o PageFile) e pode ser criado sob demanda.
    [[nodiscard]] BlobStore blobs() noexcept { return BlobStore{*file_}; }

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

    // Grava o buffer da transação no WAL, aplica-o e (em full) remove o WAL.
    [[nodiscard]] Result<void> commit_transaction(CommitPhase phase);
    // Descarta o buffer da transação e remove qualquer WAL residual.
    [[nodiscard]] Result<void> rollback_transaction();
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

    friend class DatabaseRegistry;
    void set_database_id(DatabaseId id) noexcept { database_id_ = id; }

    std::unique_ptr<storage::PageFile> file_;
    ObjectStore store_;
    std::unordered_map<const void*, BoundType> bound_;
    std::unordered_map<std::string, std::unordered_map<std::uint64_t, Migration>> migrations_;
    DatabaseId database_id_{};
    // Caminho do write-ahead log (`<db>.wal`).
    std::filesystem::path wal_path_;
    // Id da transação corrente e o próximo a atribuir (monotônico por sessão).
    std::uint64_t current_tx_id_{0};
    std::uint64_t next_tx_id_{1};
    // Fábrica do arquivo do WAL; produção usa NativeFile, testes injetam falhas.
    tx::WalFileFactory wal_factory_{tx::open_native_wal_sink};
};

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
