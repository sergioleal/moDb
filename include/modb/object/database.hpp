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

class Transaction {
public:
    [[nodiscard]] DatabaseId database() const noexcept { return database_; }

private:
    friend class Database;
    explicit Transaction(DatabaseId database) noexcept : database_{database} {}
    DatabaseId database_;
};

class Database;

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
        auto type_id = register_or_adopt(*binding);
        if (!type_id) {
            return std::unexpected(type_id.error());
        }
        bound_.insert_or_assign(type_key<T>(), BoundType{std::move(*binding), *type_id});
        return {};
    }

    // Persiste um objeto C++ e devolve seu Handle. O tipo precisa estar bound.
    template <typename T>
    [[nodiscard]] Result<Handle<T>> create(const T& value) {
        if (database_id_.value == 0) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "database must be attached before use"});
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
    template <typename T>
    [[nodiscard]] Result<void> update(const Handle<T>& handle, const T& value) {
        if (database_id_.value == 0 || handle.database() != database_id_) {
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

    [[nodiscard]] Transaction begin() const noexcept { return Transaction{database_id_}; }

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

    // Remove um objeto pelo id (o id nunca é reutilizado). Referências de
    // composição (OwnedRef) são removidas em cascata, em profundidade-primeiro;
    // um ciclo de posse é detectado e falha com invalid_argument (ADR-008).
    // Referências simples (Ref) não são seguidas: o alvo some e a resolução
    // posterior falha com record_not_found (referência pendente detectável).
    [[nodiscard]] Result<void> remove(ObjectId id);

    // Persiste no dispositivo tudo que foi escrito (durabilidade real).
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

    Database(std::unique_ptr<storage::PageFile> file, ObjectStore store)
        : file_{std::move(file)}, store_{std::move(store)} {}

    // Remove um objeto e, recursivamente, os filhos referenciados por OwnedRef.
    // `visited` guarda os ids em andamento para detectar ciclos de posse.
    [[nodiscard]] Result<void> remove_cascade(ObjectId id,
                                              std::unordered_set<std::uint64_t>& visited);

    // Registra o tipo do binding ou adota o já persistido (validando estrutura).
    [[nodiscard]] Result<TypeDefinitionId> register_or_adopt(const Binding& binding);
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
    return (*database)->update(*this, *object);
}

} // namespace modb::object
