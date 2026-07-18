// Importa a interface de ObjectStore.
#include "modb/object/object_store.hpp"

// Importa a B+ tree e o codec de chave ordenável (Fase 7B).
#include "modb/index/btree.hpp"
#include "modb/index/key_codec.hpp"

// Disponibiliza std::min no comparador de bytes.
#include <algorithm>
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
    if (identity->directory_root() != *identity_dir) {
        // A migração v1→v2 publica a nova raiz por último. Se houver queda
        // antes daqui, a raiz antiga continua íntegra e a abertura apenas
        // refaz a migração; depois daqui o mapa v2 já está completo.
        if (auto linked = root->set_identity_dir(identity->directory_root()); !linked) {
            return std::unexpected(linked.error());
        }
        if (auto flushed = file.flush(); !flushed) {
            return std::unexpected(flushed.error());
        }
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

    // Diretório de índices (Fase 7B): carregado do DBRT se já existir.
    std::optional<IndexCatalog> indexes;
    if (auto dir = root->index_dir()) {
        auto opened = IndexCatalog::open(file, *dir);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        indexes = std::move(*opened);
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
                       std::move(current),
                       std::move(indexes)};
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
    // A época deste objeto é a do commit que o tornará durável (Fase 6B): a
    // mesma que advance_epoch() vai publicar, já que só há um escritor.
    if (auto bound = identity_.bind(*id, *location, root_.epoch() + 1); !bound) {
        return std::unexpected(bound.error());
    }
    // Manutenção de índices (Fase 7B): a mesma transação cobre objeto e índices.
    if (auto indexed = index_maintain(std::string{type.name()}, *id, fields, /*insert=*/true);
        !indexed) {
        return std::unexpected(indexed.error());
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

Result<DecodedObject> ObjectStore::get_at(ObjectId id, std::uint64_t snapshot_epoch) {
    auto location = identity_.find_at(id, snapshot_epoch);
    if (!location) {
        return std::unexpected(location.error());
    }
    auto record = data_heap_.read(*location);
    if (!record) {
        return std::unexpected(record.error());
    }
    return decode_object(*record);
}

Result<void> ObjectStore::check_snapshot_conflict(
    ObjectId id, std::optional<std::uint64_t> oldest_open_snapshot_epoch) const {
    if (!oldest_open_snapshot_epoch) {
        return {};
    }
    auto current = identity_.current_epoch(id);
    if (!current) {
        return std::unexpected(current.error());
    }
    // Nenhum snapshot aberto precisa de algo anterior à época current: seguro.
    if (*oldest_open_snapshot_epoch >= *current) {
        return {};
    }
    auto occupied = identity_.has_previous(id);
    if (!occupied) {
        return std::unexpected(occupied.error());
    }
    if (*occupied) {
        return std::unexpected(Error{
            ErrorCode::snapshot_conflict,
            "object " + std::to_string(id.value) +
                " already has a previous version visible to an older open snapshot"});
    }
    return {};
}

Result<void> ObjectStore::update(ObjectId id, const TypeDefinition& type, FieldValues fields,
                                 std::optional<std::uint64_t> oldest_open_snapshot_epoch) {
    if (!file_->in_transaction()) {
        return std::unexpected(
            Error{ErrorCode::transaction_required, "object update requires an active transaction"});
    }
    if (auto valid = validate_object(type, fields); !valid) {
        return std::unexpected(valid.error());
    }
    // Confere ANTES de qualquer escrita física: um conflito não deixa rastro.
    if (auto conflict = check_snapshot_conflict(id, oldest_open_snapshot_epoch); !conflict) {
        return std::unexpected(conflict.error());
    }
    // Captura os valores antigos ANTES de sobrescrever, para retirar as chaves
    // antigas dos índices (Fase 7B). Só quando há índices.
    std::optional<DecodedObject> previous;
    if (indexes_) {
        if (auto existing = get(id); existing) {
            previous = std::move(*existing);
        }
    }
    auto record = encode_object(id, type.id(), fields);
    if (!record) {
        return std::unexpected(record.error());
    }
    // Nunca reaproveita o registro antigo: um snapshot aberto pode depender
    // dos bytes exatamente como estavam antes desta escrita (Fase 6B). A nova
    // versão sempre ocupa um endereço físico próprio; o antigo permanece
    // legível como `previous` até a Fase 6C decidir reciclá-lo.
    auto location = data_heap_.insert(*record);
    if (!location) {
        return std::unexpected(location.error());
    }
    if (auto rebound = identity_.rebind(id, *location, root_.epoch() + 1); !rebound) {
        return std::unexpected(rebound.error());
    }
    // Atualiza os índices: retira as chaves antigas e insere as novas.
    if (previous) {
        if (auto r = index_maintain(std::string{type.name()}, id, previous->fields,
                                    /*insert=*/false);
            !r) {
            return std::unexpected(r.error());
        }
    }
    return index_maintain(std::string{type.name()}, id, fields, /*insert=*/true);
}

Result<void> ObjectStore::remove(ObjectId id,
                                 std::optional<std::uint64_t> oldest_open_snapshot_epoch) {
    if (!file_->in_transaction()) {
        return std::unexpected(
            Error{ErrorCode::transaction_required, "object removal requires an active transaction"});
    }
    if (auto conflict = check_snapshot_conflict(id, oldest_open_snapshot_epoch); !conflict) {
        return std::unexpected(conflict.error());
    }
    // Captura o objeto antes de remover, para retirar suas chaves dos índices.
    std::optional<DecodedObject> previous;
    if (indexes_) {
        if (auto existing = get(id); existing) {
            previous = std::move(*existing);
        }
    }
    // O registro físico não é tocado: um snapshot mais antigo pode precisar
    // dele via `previous`. A reciclagem do espaço é responsabilidade da Fase
    // 6C (retenção e GC), não desta escrita.
    if (auto erased = identity_.erase(id, root_.epoch() + 1); !erased) {
        return std::unexpected(erased.error());
    }
    if (previous) {
        auto type = find_type(previous->type);
        if (type) {
            if (auto r = index_maintain(std::string{type->get().name()}, id, previous->fields,
                                        /*insert=*/false);
                !r) {
                return std::unexpected(r.error());
            }
        }
    }
    return {};
}

namespace {

// Localiza o valor de um campo dentro dos valores de um objeto; nullptr se
// ausente.
const AttributeValue* field_value(const FieldValues& fields, std::uint16_t field_id) {
    for (const auto& [id, value] : fields) {
        if (id.value == field_id) {
            return &value;
        }
    }
    return nullptr;
}

// a < b lexicograficamente (bytes não sinalizados).
bool bytes_less(std::span<const std::byte> a, std::span<const std::byte> b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto ai = std::to_integer<std::uint8_t>(a[i]);
        const auto bi = std::to_integer<std::uint8_t>(b[i]);
        if (ai != bi) {
            return ai < bi;
        }
    }
    return a.size() < b.size();
}

} // namespace

Result<void> ObjectStore::index_maintain(const std::string& type_name, ObjectId id,
                                         const FieldValues& fields, bool insert) {
    if (!indexes_) {
        return {};
    }
    for (std::size_t slot = 0; slot < indexes_->indexes().size(); ++slot) {
        const auto& info = indexes_->indexes()[slot];
        if (info.type_name != type_name) {
            continue;
        }
        const AttributeValue* value = field_value(fields, info.field_id);
        if (value == nullptr || value->is_null()) {
            continue;  // nulos não entram no índice
        }
        auto key = index::encode_key(*value);
        if (!key) {
            return std::unexpected(key.error());
        }
        auto tree = index::BTree::open(*file_, info.root);
        if (!tree) {
            return std::unexpected(tree.error());
        }
        if (insert) {
            if (auto r = tree->insert(*key, id.value); !r) {
                return std::unexpected(r.error());
            }
        } else if (auto r = tree->remove(*key, id.value);
                   !r && r.error().code != ErrorCode::record_not_found) {
            return std::unexpected(r.error());
        }
        // A raiz muda quando a árvore cresce/encolhe: persiste a nova.
        if (tree->root_page() != info.root) {
            if (auto r = indexes_->set_root(slot, tree->root_page()); !r) {
                return std::unexpected(r.error());
            }
        }
    }
    return {};
}

Result<void> ObjectStore::create_index(const TypeDefinition& type, FieldId field) {
    if (!file_->in_transaction()) {
        return std::unexpected(
            Error{ErrorCode::transaction_required, "creating an index requires a transaction"});
    }
    if (type.find(field) == nullptr) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "type has no such field to index"});
    }
    if (!indexes_) {
        auto dir = IndexCatalog::create(*file_);
        if (!dir) {
            return std::unexpected(dir.error());
        }
        if (auto linked = root_.set_index_dir(dir->directory()); !linked) {
            return std::unexpected(linked.error());
        }
        indexes_ = std::move(*dir);
    }
    if (indexes_->find(type.name(), field.value) >= 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "index already exists"});
    }

    auto tree = index::BTree::create(*file_);
    if (!tree) {
        return std::unexpected(tree.error());
    }
    // Backfill: indexa os objetos CURRENT do tipo (todas as versões de schema).
    auto records = data_heap_.scan_records();
    if (!records) {
        return std::unexpected(records.error());
    }
    for (const auto& record : *records) {
        auto object = decode_object(record.bytes);
        if (!object) {
            return std::unexpected(object.error());
        }
        auto current = identity_.find(object->id);
        if (!current || *current != record.id) {
            continue;  // versão previous ou órfã
        }
        auto object_type = find_type(object->type);
        if (!object_type || object_type->get().name() != type.name()) {
            continue;  // outro tipo lógico
        }
        const AttributeValue* value = field_value(object->fields, field.value);
        if (value == nullptr || value->is_null()) {
            continue;
        }
        auto key = index::encode_key(*value);
        if (!key) {
            return std::unexpected(key.error());
        }
        if (auto r = tree->insert(*key, object->id.value); !r) {
            return std::unexpected(r.error());
        }
    }
    return indexes_->add(IndexInfo{std::string{type.name()}, field.value, tree->root_page()});
}

bool ObjectStore::has_index(std::string_view type_name, std::uint16_t field_id) const noexcept {
    return indexes_.has_value() && indexes_->find(type_name, field_id) >= 0;
}

Result<std::vector<ObjectId>> ObjectStore::index_equal(std::string_view type_name,
                                                       std::uint16_t field_id,
                                                       const AttributeValue& key) const {
    return index_range(type_name, field_id, key, key);
}

Result<std::vector<ObjectId>> ObjectStore::index_range(std::string_view type_name,
                                                       std::uint16_t field_id,
                                                       const AttributeValue& lo,
                                                       const AttributeValue& hi) const {
    if (!indexes_) {
        return std::unexpected(Error{ErrorCode::type_not_found, "no index on this field"});
    }
    const int slot = indexes_->find(type_name, field_id);
    if (slot < 0) {
        return std::unexpected(Error{ErrorCode::type_not_found, "no index on this field"});
    }
    auto lo_key = index::encode_key(lo);
    if (!lo_key) {
        return std::unexpected(lo_key.error());
    }
    auto hi_key = index::encode_key(hi);
    if (!hi_key) {
        return std::unexpected(hi_key.error());
    }
    auto tree = index::BTree::open(*file_, indexes_->indexes()[static_cast<std::size_t>(slot)].root);
    if (!tree) {
        return std::unexpected(tree.error());
    }
    auto ids = tree->range(*lo_key, *hi_key);
    if (!ids) {
        return std::unexpected(ids.error());
    }
    std::vector<ObjectId> out;
    out.reserve(ids->size());
    for (auto value : *ids) {
        out.push_back(ObjectId{value});
    }
    return out;
}

Result<std::optional<DecodedObject>> ObjectStore::index_candidate_at(
    ObjectId id, std::uint64_t snapshot_epoch, std::uint16_t field_id, const AttributeValue& lo,
    const AttributeValue& hi) {
    auto object = get_at(id, snapshot_epoch);
    if (!object) {
        if (object.error().code == ErrorCode::record_not_found) {
            return std::optional<DecodedObject>{};  // não visível nesta época
        }
        return std::unexpected(object.error());
    }
    const AttributeValue* value = field_value(object->fields, field_id);
    if (value == nullptr || value->is_null()) {
        return std::optional<DecodedObject>{};
    }
    auto enc = index::encode_key(*value);
    if (!enc) {
        return std::unexpected(enc.error());
    }
    auto lo_key = index::encode_key(lo);
    if (!lo_key) {
        return std::unexpected(lo_key.error());
    }
    auto hi_key = index::encode_key(hi);
    if (!hi_key) {
        return std::unexpected(hi_key.error());
    }
    if (bytes_less(*enc, *lo_key) || bytes_less(*hi_key, *enc)) {
        return std::optional<DecodedObject>{};  // fora da faixa na versão do snapshot
    }
    return std::optional<DecodedObject>{std::move(*object)};
}

Result<std::size_t> ObjectStore::collect_garbage(
    std::optional<std::uint64_t> oldest_open_snapshot_epoch) {
    if (!file_->in_transaction()) {
        return std::unexpected(Error{ErrorCode::transaction_required,
                                     "garbage collection requires an active transaction"});
    }
    auto records = data_heap_.scan_records();
    if (!records) {
        return std::unexpected(records.error());
    }

    // Um alvo a reciclar: o registro físico e se ele é a `previous` referenciada
    // (que também precisa ter o slot compactado) ou uma cópia órfã (só liberar).
    struct Victim {
        storage::RecordId record;
        ObjectId id;
        bool is_referenced_previous;
    };
    std::vector<Victim> victims;
    // Classifica lendo a identidade ANTES de qualquer mutação, para a decisão
    // de cada registro refletir o estado consistente do início do GC.
    for (const auto& record : *records) {
        auto object = decode_object(record.bytes);
        if (!object) {
            return std::unexpected(object.error());
        }
        auto info = identity_.inspect(object->id);
        if (!info) {
            // Sem entrada de identidade: nenhum id vivo reivindica este registro.
            if (info.error().code == ErrorCode::record_not_found) {
                victims.push_back({record.id, object->id, false});
                continue;
            }
            return std::unexpected(info.error());
        }
        // A versão current viva nunca é reciclada.
        if (info->current && *info->current == record.id) {
            continue;
        }
        // A versão previous referenciada: reciclável só quando nenhum snapshot
        // aberto ainda pode enxergá-la (mesma regra segura do conflito da 6B:
        // basta o snapshot mais antigo já ser >= à época current da entrada).
        if (info->previous && *info->previous == record.id) {
            const bool collectible =
                !oldest_open_snapshot_epoch || *oldest_open_snapshot_epoch >= info->current_epoch;
            if (collectible) {
                victims.push_back({record.id, object->id, true});
            }
            continue;
        }
        // Nem current nem a previous referenciada: cópia antiga órfã (ex.: um
        // previous sobrescrito por uma segunda alteração). Sempre reciclável.
        victims.push_back({record.id, object->id, false});
    }

    std::size_t collected = 0;
    for (const auto& victim : victims) {
        if (victim.is_referenced_previous) {
            if (auto cleared = identity_.clear_previous(victim.id); !cleared) {
                return std::unexpected(cleared.error());
            }
        }
        if (auto erased = data_heap_.erase(victim.record); !erased) {
            // Um registro já ausente não é um erro para o GC; qualquer outra
            // falha (ex.: página corrompida) aborta a coleta.
            if (erased.error().code != ErrorCode::record_not_found) {
                return std::unexpected(erased.error());
            }
        }
        ++collected;
    }
    return collected;
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
        // Desde a Fase 6B, update() nunca reaproveita o registro antigo (e
        // remove() nunca apaga o físico): ambos preservam a versão anterior
        // como `previous`, para snapshots abertos. Isso significa que o heap
        // físico pode conter, para o mesmo id, tanto o registro current quanto
        // um previous órfão — só o que `identity_.find` resolve como current
        // é visitado aqui; o resto é uma versão antiga preservada, não um
        // objeto vivo.
        auto current = identity_.find(object->id);
        if (!current || *current != record.id) {
            continue;
        }
        if (auto result = visitor(*object); !result) {
            return std::unexpected(result.error());
        }
    }
    return {};
}

Result<void> ObjectStore::scan_at(
    std::uint64_t snapshot_epoch,
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
        // O heap físico pode conter, para o mesmo id, tanto a versão current
        // quanto uma previous ainda preservada. Só o registro cuja localização
        // é exatamente a que find_at resolveria para esta época é visitado —
        // isso evita tanto duplicar o objeto quanto expor a versão errada.
        auto resolved = identity_.find_at(object->id, snapshot_epoch);
        if (!resolved || *resolved != record.id) {
            continue;
        }
        if (auto result = visitor(*object); !result) {
            return std::unexpected(result.error());
        }
    }
    return {};
}

query::Generator<Result<DecodedObject>> ObjectStore::scan_stream(
    std::uint64_t snapshot_epoch, std::optional<TypeDefinitionId> type) {
    // Percorre a cadeia de páginas de dados sob demanda: cada iteração do while
    // lê exatamente UMA página (read_page_records) e cede seus objetos visíveis;
    // a próxima página só é lida quando o consumidor esgota esta. Um `limit` a
    // montante encerra o fluxo destruindo esta coroutine — nenhuma página além
    // das necessárias é lida.
    auto page = data_heap_.first_page();
    while (page) {
        auto slice = data_heap_.read_page_records(*page);
        if (!slice) {
            co_yield std::unexpected(slice.error());
            co_return;
        }
        for (auto& record : slice->records) {
            auto object = decode_object(record.bytes);
            if (!object) {
                co_yield std::unexpected(object.error());
                co_return;
            }
            if (type && object->type != *type) {
                continue;
            }
            // Só a versão que `find_at` resolve para esta época é visível: o
            // resto do heap são versões previous preservadas ou órfãos.
            auto resolved = identity_.find_at(object->id, snapshot_epoch);
            if (!resolved || *resolved != record.id) {
                continue;
            }
            co_yield Result<DecodedObject>{std::move(*object)};
        }
        page = slice->next;
    }
}

} // namespace modb::object
