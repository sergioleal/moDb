// Importa a interface de Database.
#include "modb/object/database.hpp"

// Importa a recuperação executada na abertura.
#include "modb/tx/recovery.hpp"
// Importa o WAL usado no commit.
#include "modb/tx/wal.hpp"

// Disponibiliza std::error_code na remoção do WAL.
#include <system_error>
// Disponibiliza o conjunto de ids visitados na cascata.
#include <unordered_set>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {
namespace {

// Deriva o caminho do WAL a partir do caminho do banco (`<db>.wal`).
std::filesystem::path wal_path_for(const std::filesystem::path& path) {
    std::filesystem::path wal = path;
    wal += ".wal";
    return wal;
}

// Compara duas TypeDefinitions ignorando o id (que difere entre a canônica do
// binding, ainda sem id, e a persistida). Confere nome e atributos.
bool same_structure(const TypeDefinition& stored, const TypeDefinition& canonical) {
    if (stored.name() != canonical.name()) {
        return false;
    }
    if (stored.attributes().size() != canonical.attributes().size()) {
        return false;
    }
    for (const auto& attribute : canonical.attributes()) {
        const auto* candidate = stored.find(attribute.id);
        if (candidate == nullptr || *candidate != attribute) {
            return false;
        }
    }
    return true;
}

} // namespace

Result<Database> Database::create(const std::filesystem::path& path) {
    auto page_file = storage::PageFile::create(path);
    if (!page_file) {
        return std::unexpected(page_file.error());
    }
    // Endereço estável: o ObjectStore guardará um PageFile* para dentro dele.
    auto file = std::make_unique<storage::PageFile>(std::move(*page_file));
    auto store = ObjectStore::create(*file);
    if (!store) {
        return std::unexpected(store.error());
    }
    // Um banco novo não deve herdar um WAL residual de um banco homônimo antigo.
    auto wal_path = wal_path_for(path);
    std::error_code remove_error;
    std::filesystem::remove(wal_path, remove_error);
    return Database{std::move(file), std::move(*store), std::move(wal_path)};
}

Result<Database> Database::open(const std::filesystem::path& path) {
    auto page_file = storage::PageFile::open(path);
    if (!page_file) {
        return std::unexpected(page_file.error());
    }
    auto file = std::make_unique<storage::PageFile>(std::move(*page_file));
    // Recuperação antes de reconstruir o catálogo: reaplica transações
    // commitadas do WAL para que o ObjectStore leia o estado consolidado.
    auto wal_path = wal_path_for(path);
    if (auto recovered = tx::recover(*file, wal_path); !recovered) {
        return std::unexpected(recovered.error());
    }
    auto store = ObjectStore::open(*file);
    if (!store) {
        return std::unexpected(store.error());
    }
    return Database{std::move(file), std::move(*store), std::move(wal_path)};
}

Result<TypeDefinitionId> Database::register_or_adopt(const Binding& binding) {
    auto canonical = binding.to_type_definition();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    // Já existe um tipo com esse nome? Adota se a estrutura for idêntica;
    // caso contrário, registra uma nova versão e baseline.
    if (auto existing = store_.find_type(binding.type_name())) {
        if (same_structure(existing->get(), *canonical)) {
            return existing->get().id();
        }
        return store_.register_type(std::move(*canonical));
    }
    // Tipo novo: registra a definição canônica do binding.
    return store_.register_type(std::move(*canonical));
}

Result<TypeDefinitionId> Database::persist_binding(const Binding& binding) {
    // Se o chamador já abriu uma transação, participa dela (sem commit próprio).
    if (file_->in_transaction()) {
        return register_or_adopt(binding);
    }
    // Caso contrário, envolve as escritas de catálogo numa transação interna,
    // para que registrar/evoluir um tipo seja atômico e passe pelo WAL.
    current_tx_id_ = next_tx_id_++;
    file_->begin_transaction();
    auto type_id = register_or_adopt(binding);
    if (!type_id) {
        (void)rollback_transaction();
        return std::unexpected(type_id.error());
    }
    if (auto committed = commit_transaction(CommitPhase::full); !committed) {
        (void)rollback_transaction();
        return std::unexpected(committed.error());
    }
    return type_id;
}

const Database::BoundType* Database::bound_for(const void* type) const {
    const auto it = bound_.find(type);
    return it == bound_.end() ? nullptr : &it->second;
}

Result<void> Database::remove(Transaction& tx, ObjectId id) {
    if (auto writable = check_writable(tx); !writable) {
        return std::unexpected(writable.error());
    }
    std::unordered_set<std::uint64_t> visited;
    return remove_cascade(id, visited);
}

Result<Transaction> Database::begin() {
    if (database_id_.value == 0) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "database must be attached before begin"});
    }
    if (file_->in_transaction()) {
        return std::unexpected(
            Error{ErrorCode::transaction_active, "a transaction is already in progress"});
    }
    current_tx_id_ = next_tx_id_++;
    file_->begin_transaction();
    return Transaction{database_id_};
}

Result<void> Database::commit_transaction(CommitPhase phase) {
    if (!file_->in_transaction()) {
        return std::unexpected(
            Error{ErrorCode::transaction_required, "no active transaction to commit"});
    }
    {
        // Mantém o descritor do WAL em um escopo próprio. Ele precisa estar
        // fechado antes do checkpoint removê-lo, especialmente no Windows.
        // 1. Escreve begin + imagens de página no WAL e o torna durável. A
        // fábrica do arquivo é injetável: produção abre um NativeFile; testes
        // injetam um FailpointFile que falha após N escritas (failpoints).
        auto wal = tx::Wal::create(wal_path_, wal_factory_);
        if (!wal) {
            return std::unexpected(wal.error());
        }
        if (auto appended = wal->append_begin(current_tx_id_); !appended) {
            return std::unexpected(appended.error());
        }
        for (const auto& [page_id, page] : file_->transaction_pages()) {
            if (auto appended =
                    wal->append_page_image(current_tx_id_, storage::PageId{page_id}, page.bytes());
                !appended) {
                return std::unexpected(appended.error());
            }
        }
        if (auto synced = wal->sync(); !synced) {
            return std::unexpected(synced.error());
        }
        // Costura de failpoint: parar aqui deixa o WAL sem registro de commit; a
        // recuperação descarta a transação (tudo-ou-nada → nada).
        if (phase == CommitPhase::stop_after_images) {
            return {};
        }
        // 2. Registro de commit + sync: a partir daqui a transação é durável.
        if (auto appended = wal->append_commit(current_tx_id_); !appended) {
            return std::unexpected(appended.error());
        }
        if (auto synced = wal->sync(); !synced) {
            return std::unexpected(synced.error());
        }
        // Costura: parar aqui simula queda após o commit durável, antes de aplicar;
        // a recuperação reaplica (tudo-ou-nada → tudo).
        if (phase == CommitPhase::stop_after_commit_record) {
            return {};
        }
        // 3. Aplica as páginas ao arquivo de dados e as torna duráveis.
        if (auto applied = file_->apply_transaction(); !applied) {
            return std::unexpected(applied.error());
        }
        if (auto flushed = file_->flush(); !flushed) {
            return std::unexpected(flushed.error());
        }
        // Costura: parar aqui mantém o WAL; a recuperação reaplica de forma
        // idempotente e então o remove.
        if (phase == CommitPhase::stop_before_wal_cleanup) {
            return {};
        }
    }
    // 4. Checkpoint: as páginas já estão duráveis, o WAL pode ser removido.
    std::error_code remove_error;
    std::filesystem::remove(wal_path_, remove_error);
    return {};
}

Result<void> Database::rollback_transaction() {
    file_->discard_transaction();
    std::error_code remove_error;
    std::filesystem::remove(wal_path_, remove_error);
    return {};
}

Transaction::~Transaction() {
    if (active_) {
        (void)rollback();
    }
}

Result<void> Transaction::commit() { return commit(CommitPhase::full); }

Result<void> Transaction::commit(CommitPhase phase) {
    auto database = DatabaseRegistry::instance().find(database_);
    if (!database) {
        // Sem banco não há como reverter nem aplicar; consome a transação.
        active_ = false;
        return std::unexpected(database.error());
    }
    auto committed = (*database)->commit_transaction(phase);
    // Só consome a transação quando o commit conclui (ou para num failpoint
    // intencional, que também retorna Ok). Se o commit falhar de verdade,
    // `active_` permanece e o destrutor executa o rollback — nunca deixando o
    // PageFile preso numa transação nem o buffer aplicado pela metade.
    if (committed) {
        active_ = false;
    }
    return committed;
}

Result<void> Transaction::rollback() {
    active_ = false;
    auto database = DatabaseRegistry::instance().find(database_);
    if (!database) {
        return {};
    }
    return (*database)->rollback_transaction();
}

Result<void> Database::remove_cascade(ObjectId id, std::unordered_set<std::uint64_t>& visited) {
    // Um id revisitado significa posse cíclica (A◆B◆A) ou posse compartilhada,
    // ambas inválidas para composição: falha antes de remover qualquer coisa
    // ainda pendente na pilha de recursão.
    if (!visited.insert(id.value).second) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "cycle detected in owned-reference cascade"});
    }
    auto object = store_.get(id);
    if (!object) {
        return std::unexpected(object.error());
    }
    auto type = store_.find_type(object->type);
    if (!type) {
        return std::unexpected(type.error());
    }
    // Remove primeiro os filhos de composição (profundidade-primeiro), depois o
    // próprio objeto — assim uma falha no meio não deixa um pai órfão de filhos.
    for (const auto& attribute : type->get().attributes()) {
        if (!attribute.is_owned) {
            continue;
        }
        for (const auto& [field_id, value] : object->fields) {
            if (field_id != attribute.id || value.is_null()) {
                continue;
            }
            auto target = value.as_ref();
            if (!target) {
                return std::unexpected(target.error());
            }
            if (target->value != 0) {
                if (auto removed = remove_cascade(*target, visited); !removed) {
                    return std::unexpected(removed.error());
                }
            }
        }
    }
    return store_.remove(id);
}

Result<void> Database::register_migration(std::string type_name, std::uint64_t from_type_id,
                                          Migration migration) {
    if (type_name.empty() || from_type_id == 0 || !migration) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "migration requires type, source id and callback"});
    }
    migrations_[std::move(type_name)].insert_or_assign(from_type_id, std::move(migration));
    for (auto& [type, bound] : bound_) {
        (void)type;
        bound.plans.erase(from_type_id);
    }
    return {};
}

const Database::Migration* Database::migration_for(std::string_view type_name,
                                                   TypeDefinitionId from_type_id) const {
    const auto by_name = migrations_.find(std::string{type_name});
    if (by_name == migrations_.end()) {
        return nullptr;
    }
    const auto migration = by_name->second.find(from_type_id.value);
    if (migration == by_name->second.end()) {
        return nullptr;
    }
    if (!migration->second) {
        return nullptr;
    }
    return &migration->second;
}

std::mutex DatabaseRegistry::mutex_;
std::unordered_map<std::uint32_t, std::shared_ptr<Database>> DatabaseRegistry::databases_;
std::uint32_t DatabaseRegistry::next_id_{1};

DatabaseRegistry& DatabaseRegistry::instance() {
    static DatabaseRegistry registry;
    return registry;
}

Result<DatabaseId> DatabaseRegistry::attach(std::shared_ptr<Database> database) {
    if (!database) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "cannot attach a null database"});
    }
    std::scoped_lock lock{mutex_};
    for (const auto& [id, attached] : databases_) {
        if (attached == database) {
            return DatabaseId{id};
        }
    }
    if (next_id_ == 0) {
        return std::unexpected(
            Error{ErrorCode::value_too_large, "DatabaseId space exhausted"});
    }
    const DatabaseId id{next_id_++};
    database->set_database_id(id);
    databases_.emplace(id.value, std::move(database));
    return id;
}

Result<std::shared_ptr<Database>> DatabaseRegistry::find(DatabaseId id) const {
    if (id.value == 0) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "handle is not attached to a DatabaseRegistry"});
    }
    std::scoped_lock lock{mutex_};
    const auto found = databases_.find(id.value);
    if (found == databases_.end()) {
        return std::unexpected(
            Error{ErrorCode::type_not_found, "database is not attached"});
    }
    return found->second;
}

void DatabaseRegistry::detach(DatabaseId id) {
    std::scoped_lock lock{mutex_};
    const auto found = databases_.find(id.value);
    if (found != databases_.end()) {
        found->second->set_database_id(DatabaseId{});
        databases_.erase(found);
    }
}

} // namespace modb::object
