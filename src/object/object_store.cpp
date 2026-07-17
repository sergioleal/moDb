// Importa a interface de ObjectStore.
#include "modb/object/object_store.hpp"

// Disponibiliza o limite do espaço de ObjectIds.
#include <limits>
// Disponibiliza diagnósticos numéricos.
#include <string>
// Disponibiliza std::move.
#include <utility>

namespace modb::object {
namespace {

// Reduz um campo de raiz opcional a um erro claro quando ele deveria existir.
Result<storage::PageId> require_root(std::optional<storage::PageId> page, const char* what) {
    if (!page) {
        return std::unexpected(Error{ErrorCode::corrupt_file, what});
    }
    return *page;
}

} // namespace

Result<ObjectStore> ObjectStore::create(storage::PageFile& file) {
    auto root = DatabaseRoot::create(file);
    if (!root) {
        return std::unexpected(root.error());
    }
    auto identity = IdentityMap::create(file);
    if (!identity) {
        return std::unexpected(identity.error());
    }
    if (auto linked = root->set_identity_dir(identity->directory_root()); !linked) {
        return std::unexpected(linked.error());
    }
    auto data_heap = storage::TableHeap::create(file);
    if (!data_heap) {
        return std::unexpected(data_heap.error());
    }
    if (auto linked = root->set_data_heap_root(data_heap->root_page()); !linked) {
        return std::unexpected(linked.error());
    }
    auto catalog = CatalogStore::create(file);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }
    if (auto linked = root->set_catalog_heap_root(catalog->heap_root()); !linked) {
        return std::unexpected(linked.error());
    }

    return ObjectStore{file,
                       std::move(*root),
                       std::move(*identity),
                       std::move(*data_heap),
                       std::move(*catalog),
                       TypeRegistry{},
                       {},
                       {},
                       std::nullopt};
}

Result<ObjectStore> ObjectStore::open(storage::PageFile& file) {
    auto root = DatabaseRoot::open(file);
    if (!root) {
        return std::unexpected(root.error());
    }
    auto identity_dir = require_root(root->identity_dir(), "database root has no identity map");
    if (!identity_dir) {
        return std::unexpected(identity_dir.error());
    }
    auto data_root = require_root(root->data_heap_root(), "database root has no data heap");
    if (!data_root) {
        return std::unexpected(data_root.error());
    }
    auto catalog_root = require_root(root->catalog_heap_root(), "database root has no catalog heap");
    if (!catalog_root) {
        return std::unexpected(catalog_root.error());
    }

    auto identity = IdentityMap::open(file, *identity_dir);
    if (!identity) {
        return std::unexpected(identity.error());
    }
    auto data_heap = storage::TableHeap::open(file, *data_root);
    if (!data_heap) {
        return std::unexpected(data_heap.error());
    }
    auto catalog = CatalogStore::open(file, *catalog_root);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }

    // Reconstrói o registro de tipos a partir do heap de catálogo.
    auto contents = catalog->load_all();
    if (!contents) {
        return std::unexpected(contents.error());
    }
    TypeRegistry registry;
    for (auto& decoded : contents->types) {
        if (auto registered = registry.register_with_id(decoded.id, std::move(decoded.definition));
            !registered) {
            return std::unexpected(registered.error());
        }
    }

    // Seleciona a baseline corrente indicada pelo DBRT, se houver.
    std::optional<Baseline> current;
    std::vector<Baseline> baselines;
    baselines.reserve(contents->baselines.size());
    const auto current_id = root->current_baseline();
    if (current_id != invalid_object_id) {
        for (auto& decoded : contents->baselines) {
            auto baseline = decoded.baseline.with_id(decoded.id);
            if (decoded.id == current_id) {
                current = baseline;
            }
            baselines.push_back(std::move(baseline));
        }
        if (!current) {
            return std::unexpected(Error{ErrorCode::corrupt_file,
                                         "database root points to a missing baseline"});
        }
    }
    if (auto activated =
            registry.activate(current ? current->types() : std::span<const TypeDefinitionId>{});
        !activated) {
        return std::unexpected(activated.error());
    }

    return ObjectStore{file,
                       std::move(*root),
                       std::move(*identity),
                       std::move(*data_heap),
                       std::move(*catalog),
                       std::move(registry),
                       current ? std::vector<TypeDefinitionId>{current->types().begin(),
                                                              current->types().end()}
                               : std::vector<TypeDefinitionId>{},
                       std::move(baselines),
                       std::move(current)};
}

Result<ObjectId> ObjectStore::allocate_object_id() {
    const auto id = root_.next_object_id();
    if (id == 0 || id == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(
            Error{ErrorCode::value_too_large, "ObjectId space exhausted"});
    }
    // Persiste o contador ANTES de qualquer gravação do objeto: um crash entre
    // as duas escritas desperdiça um id, nunca o reutiliza (ADR-001/ADR-004).
    if (auto advanced = root_.set_next_object_id(id + 1); !advanced) {
        return std::unexpected(advanced.error());
    }
    return ObjectId{id};
}

Result<TypeDefinitionId> ObjectStore::register_type(TypeDefinition definition) {
    const auto previous = registry_.find(definition.name());
    const std::optional<TypeDefinitionId> previous_id =
        previous ? std::optional<TypeDefinitionId>{previous->get().id()} : std::nullopt;

    auto id = allocate_object_id();
    if (!id) {
        return std::unexpected(id.error());
    }
    const TypeDefinitionId type_id{id->value};
    auto candidate_registry = registry_;
    if (auto registered = candidate_registry.register_with_id(type_id, std::move(definition));
        !registered) {
        return std::unexpected(registered.error());
    }
    auto candidate_type_ids = type_ids_;
    if (previous_id) {
        bool replaced = false;
        for (auto& current_id : candidate_type_ids) {
            if (current_id == *previous_id) {
                current_id = type_id;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            return std::unexpected(
                Error{ErrorCode::corrupt_file, "active type is absent from current baseline"});
        }
    } else {
        candidate_type_ids.push_back(type_id);
    }

    // Persiste o tipo recém-estampado.
    auto stored = candidate_registry.find(type_id);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    if (auto saved = catalog_.save_type(stored->get()); !saved) {
        return std::unexpected(saved.error());
    }

    // Cria e persiste uma nova baseline com todos os tipos atuais.
    auto baseline = Baseline::create(candidate_type_ids);
    if (!baseline) {
        return std::unexpected(baseline.error());
    }
    auto baseline_id = allocate_object_id();
    if (!baseline_id) {
        return std::unexpected(baseline_id.error());
    }
    const auto stamped = baseline->with_id(BaselineId{baseline_id->value});
    if (auto saved = catalog_.save_baseline(stamped); !saved) {
        return std::unexpected(saved.error());
    }
    if (auto linked = root_.set_current_baseline(BaselineId{baseline_id->value}); !linked) {
        return std::unexpected(linked.error());
    }
    // O ponteiro da raiz é o commit point. Só agora publica o novo catálogo em
    // memória; tipos/baselines órfãos de uma falha anterior não ficam ativos.
    registry_ = std::move(candidate_registry);
    type_ids_ = std::move(candidate_type_ids);
    current_baseline_ = stamped;
    baselines_.push_back(stamped);

    return type_id;
}

Result<std::reference_wrapper<const TypeDefinition>> ObjectStore::find_type(
    TypeDefinitionId id) const {
    return registry_.find(id);
}

Result<std::reference_wrapper<const TypeDefinition>> ObjectStore::find_type(
    std::string_view name) const {
    return registry_.find(name);
}

Result<std::reference_wrapper<const Baseline>> ObjectStore::find_baseline(BaselineId id) const {
    for (const auto& baseline : baselines_) {
        if (baseline.id() == id) {
            return std::cref(baseline);
        }
    }
    return std::unexpected(
        Error{ErrorCode::type_not_found, "baseline not found: " + std::to_string(id.value)});
}

Result<ObjectId> ObjectStore::create_object(const TypeDefinition& type, FieldValues fields) {
    if (!file_->in_transaction()) {
        return std::unexpected(
            Error{ErrorCode::transaction_required, "object creation requires an active transaction"});
    }
    // O tipo precisa estar registrado (id atribuído) para ter um id persistente.
    if (type.id() == invalid_object_id) {
        return std::unexpected(
            Error{ErrorCode::type_not_found, "cannot create an object of an unregistered type"});
    }
    if (auto valid = validate_object(type, fields); !valid) {
        return std::unexpected(valid.error());
    }

    auto id = allocate_object_id();
    if (!id) {
        return std::unexpected(id.error());
    }
    auto record = encode_object(*id, type.id(), fields);
    if (!record) {
        return std::unexpected(record.error());
    }
    auto location = data_heap_.insert(*record);
    if (!location) {
        return std::unexpected(location.error());
    }
    if (auto bound = identity_.bind(*id, *location); !bound) {
        return std::unexpected(bound.error());
    }
    return *id;
}

Result<DecodedObject> ObjectStore::get(ObjectId id) {
    auto location = identity_.find(id);
    if (!location) {
        return std::unexpected(location.error());
    }
    auto record = data_heap_.read(*location);
    if (!record) {
        return std::unexpected(record.error());
    }
    return decode_object(*record);
}

Result<void> ObjectStore::update(ObjectId id, const TypeDefinition& type, FieldValues fields) {
    if (!file_->in_transaction()) {
        return std::unexpected(
            Error{ErrorCode::transaction_required, "object update requires an active transaction"});
    }
    if (auto valid = validate_object(type, fields); !valid) {
        return std::unexpected(valid.error());
    }
    auto location = identity_.find(id);
    if (!location) {
        return std::unexpected(location.error());
    }
    auto record = encode_object(id, type.id(), fields);
    if (!record) {
        return std::unexpected(record.error());
    }
    auto moved = data_heap_.update(*location, *record);
    if (!moved) {
        return std::unexpected(moved.error());
    }
    // Só reescreve o mapa de identidade se o registro mudou de endereço.
    if (*moved != *location) {
        if (auto rebound = identity_.rebind(id, *moved); !rebound) {
            return std::unexpected(rebound.error());
        }
    }
    return {};
}

Result<void> ObjectStore::remove(ObjectId id) {
    if (!file_->in_transaction()) {
        return std::unexpected(
            Error{ErrorCode::transaction_required, "object removal requires an active transaction"});
    }
    auto location = identity_.find(id);
    if (!location) {
        return std::unexpected(location.error());
    }
    if (auto erased = data_heap_.erase(*location); !erased) {
        return std::unexpected(erased.error());
    }
    return identity_.erase(id);
}

Result<void> ObjectStore::scan(
    const std::function<Result<void>(const DecodedObject&)>& visitor) {
    auto records = data_heap_.scan_records();
    if (!records) {
        return std::unexpected(records.error());
    }
    for (const auto& record : *records) {
        auto object = decode_object(record.bytes);
        if (!object) {
            return std::unexpected(object.error());
        }
        if (auto result = visitor(*object); !result) {
            return std::unexpected(result.error());
        }
    }
    return {};
}

} // namespace modb::object
