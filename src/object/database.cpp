// Importa a interface de Database.
#include "modb/object/database.hpp"

// Disponibiliza o conjunto de ids visitados na cascata.
#include <unordered_set>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {
namespace {

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
    return Database{std::move(file), std::move(*store)};
}

Result<Database> Database::open(const std::filesystem::path& path) {
    auto page_file = storage::PageFile::open(path);
    if (!page_file) {
        return std::unexpected(page_file.error());
    }
    auto file = std::make_unique<storage::PageFile>(std::move(*page_file));
    auto store = ObjectStore::open(*file);
    if (!store) {
        return std::unexpected(store.error());
    }
    return Database{std::move(file), std::move(*store)};
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

const Database::BoundType* Database::bound_for(const void* type) const {
    const auto it = bound_.find(type);
    return it == bound_.end() ? nullptr : &it->second;
}

Result<void> Database::remove(ObjectId id) {
    std::unordered_set<std::uint64_t> visited;
    return remove_cascade(id, visited);
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
