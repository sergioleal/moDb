// Importa o mapa de identidade exercitado neste teste.
#include "modb/object/identity_map.hpp"
// Importa PageFile para criar/reabrir o arquivo.
#include "modb/storage/page_file.hpp"
#include "modb/storage/endian.hpp"

// Importa as funções simples de verificação dos testes.
#include "test_support.hpp"

// Disponibiliza o relógio usado no nome único do arquivo.
#include <chrono>
#include <algorithm>
#include <array>
// Disponibiliza caminhos temporários.
#include <filesystem>
// Disponibiliza std::error_code na limpeza.
#include <system_error>

using namespace modb;
using namespace modb::object;
using modb::storage::PageFile;
using modb::storage::PageId;
using modb::storage::RecordId;
using modb::storage::SlotId;
using modb::storage::store_le;

namespace {

// Arquivo temporário isolado por teste, removido no destrutor.
class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-idmap-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Monta um RecordId sintético a partir de um número, só para round-trip.
RecordId make_record(std::uint64_t n) {
    return RecordId{PageId{n}, SlotId{static_cast<std::uint16_t>(n % 251)},
                    static_cast<std::uint16_t>(n % 7 + 1)};
}

} // namespace

int main() {
    TestSuite suite;

    TemporaryDatabase database{"map"};
    auto created = PageFile::create(database.path());
    suite.check(created.has_value(), "database file is created");
    if (!created) {
        return suite.finish();
    }

    {
        auto file = std::move(*created);
        auto map = IdentityMap::create(file);
        suite.check(map.has_value(), "identity map is created");
        if (!map) {
            return suite.finish();
        }

        // bind/find: a localização volta idêntica.
        const auto record = make_record(42);
        suite.check(map->bind(ObjectId{16}, record, 1).has_value(), "an object binds");
        suite.check(map->find(ObjectId{16}) == Result<RecordId>{record},
                    "a bound object is found at the same location");

        // find de id inexistente.
        suite.check_error(map->find(ObjectId{17}), ErrorCode::record_not_found,
                          "an unbound id is reported as not found");

        // rebind: atualiza a localização.
        const auto moved = make_record(99);
        suite.check(map->rebind(ObjectId{16}, moved, 2).has_value(), "a bound object rebinds");
        suite.check(map->find(ObjectId{16}) == Result<RecordId>{moved},
                    "a rebound object is found at the new location");

        // erase: find falha, rebind falha.
        suite.check(map->erase(ObjectId{16}, 3).has_value(), "a bound object is erased");
        suite.check_error(map->find(ObjectId{16}), ErrorCode::record_not_found,
                          "an erased object is not found");
        suite.check_error(map->rebind(ObjectId{16}, record, 4), ErrorCode::record_not_found,
                          "an erased object cannot be rebound");

        // Um id nunca reutilizado permanece com tombstone: novo bind do MESMO id
        // é rejeitado (o motor nunca reusa ids, mas a guarda precisa existir).
        suite.check_error(map->bind(ObjectId{16}, record, 5), ErrorCode::invalid_argument,
                          "an erased id cannot be bound again");

        // Crescimento: muitos binds forçam várias páginas IDMP.
        bool all_found = true;
        for (std::uint64_t id = 100; id < 10100; ++id) {
            if (!map->bind(ObjectId{id}, make_record(id), 1)) {
                all_found = false;
                break;
            }
        }
        suite.check(all_found, "10k binds across many IDMP pages succeed");
        bool all_correct = true;
        for (std::uint64_t id = 100; id < 10100; ++id) {
            if (map->find(ObjectId{id}) != Result<RecordId>{make_record(id)}) {
                all_correct = false;
                break;
            }
        }
        suite.check(all_correct, "every one of the 10k objects is found correctly");

        // Um id muito alto força a criação de um segundo diretório IDMD
        // encadeado (o primeiro cobre ~130k entradas com página de 4 KiB).
        const std::uint64_t high_id = 2'000'000;
        suite.check(map->bind(ObjectId{high_id}, make_record(high_id), 1).has_value(),
                    "a very high id binds, chaining a second directory");
        suite.check(map->find(ObjectId{high_id}) == Result<RecordId>{make_record(high_id)},
                    "the high id is found through the chained directory");

        suite.check(file.flush().has_value(), "map changes are flushed");
    }

    // --- Fase 6B: leitura versionada (current/previous por época) ---
    {
        TemporaryDatabase versioned_database{"versioned"};
        auto versioned_file = PageFile::create(versioned_database.path());
        suite.check(versioned_file.has_value(), "versioned fixture file is created");
        if (!versioned_file) {
            return suite.finish();
        }
        auto map = IdentityMap::create(*versioned_file);
        suite.check(map.has_value(), "versioned identity map is created");
        if (!map) {
            return suite.finish();
        }

        const auto v1 = make_record(11);
        const auto v2 = make_record(22);
        suite.check(map->bind(ObjectId{16}, v1, 1).has_value(), "versioned object binds at epoch 1");
        suite.check(map->current_epoch(ObjectId{16}) == Result<std::uint64_t>{1},
                    "current_epoch reports the binding epoch");
        suite.check(map->has_previous(ObjectId{16}) == Result<bool>{false},
                    "a freshly bound object has no previous version");

        // Antes do rebind: current e previous(inexistente) resolvem para v1
        // em qualquer época >= 1.
        suite.check(map->find_at(ObjectId{16}, 1) == Result<RecordId>{v1},
                    "find_at at the binding epoch resolves to the first version");

        suite.check(map->rebind(ObjectId{16}, v2, 5).has_value(),
                    "versioned object rebinds at epoch 5");
        suite.check(map->current_epoch(ObjectId{16}) == Result<std::uint64_t>{5},
                    "current_epoch reflects the rebind");
        suite.check(map->has_previous(ObjectId{16}) == Result<bool>{true},
                    "rebind populates the previous version");
        suite.check(map->find(ObjectId{16}) == Result<RecordId>{v2},
                    "find (no snapshot) always resolves to current");
        suite.check(map->find_at(ObjectId{16}, 5) == Result<RecordId>{v2},
                    "find_at at or after the rebind epoch resolves to the new version");
        suite.check(map->find_at(ObjectId{16}, 3) == Result<RecordId>{v1},
                    "find_at between the two epochs falls back to previous");
        suite.check_error(map->find_at(ObjectId{16}, 0), ErrorCode::record_not_found,
                          "find_at before the object was even bound reports not found");

        // Um id criado só a partir de certa época é invisível antes dela: uma
        // época anterior ao bind não encontra nem current nem previous.
        suite.check(map->bind(ObjectId{17}, make_record(33), 5).has_value(),
                    "a second object binds at epoch 5");
        suite.check_error(map->find_at(ObjectId{17}, 1), ErrorCode::record_not_found,
                          "an object created later is invisible to an older epoch");

        // erase versionado: current vira remoção, previous preserva a v2.
        suite.check(map->erase(ObjectId{16}, 9).has_value(),
                    "versioned object is removed at epoch 9");
        suite.check_error(map->find(ObjectId{16}), ErrorCode::record_not_found,
                          "find (no snapshot) does not see a removed object");
        suite.check_error(map->find_at(ObjectId{16}, 9), ErrorCode::record_not_found,
                          "find_at at or after the removal epoch does not see the object");
        suite.check(map->find_at(ObjectId{16}, 5) == Result<RecordId>{v2},
                    "find_at just before the removal still resolves via previous");
        suite.check_error(map->rebind(ObjectId{16}, v1, 10), ErrorCode::record_not_found,
                          "a removed object cannot be rebound, even under versioning");
    }

    // Reabertura: o mapa persiste entre execuções.
    {
        auto reopened = PageFile::open(database.path());
        suite.check(reopened.has_value(), "database file is reopened");
        if (!reopened) {
            return suite.finish();
        }
        // A raiz do diretório é a primeira página de dados (id 1).
        auto map = IdentityMap::open(*reopened, PageId{1});
        suite.check(map.has_value(), "identity map is reopened");
        if (map) {
            suite.check(map->find(ObjectId{5000}) == Result<RecordId>{make_record(5000)},
                        "a binding survives closing and reopening");
            suite.check(map->find(ObjectId{2'000'000}) ==
                            Result<RecordId>{make_record(2'000'000)},
                        "the chained-directory binding survives reopening");
            suite.check_error(map->find(ObjectId{16}), ErrorCode::record_not_found,
                              "the erased id stays erased after reopening");
        }
    }

    // --- migração de formato: IDMP v1 (16 bytes) -> v2 (48 bytes) ---
    {
        TemporaryDatabase legacy_database{"legacy-v1"};
        auto legacy_file = PageFile::create(legacy_database.path());
        suite.check(legacy_file.has_value(), "legacy fixture file is created");
        if (!legacy_file) {
            return suite.finish();
        }
        auto directory_id = legacy_file->allocate_page();
        auto entries_id = legacy_file->allocate_page();
        suite.check(directory_id.has_value() && entries_id.has_value(),
                    "legacy directory and entries pages are allocated");
        if (!directory_id || !entries_id) {
            return suite.finish();
        }
        constexpr std::array<std::byte, 4> idmd_magic{std::byte{'I'}, std::byte{'D'},
                                                        std::byte{'M'}, std::byte{'D'}};
        constexpr std::array<std::byte, 4> idmp_magic{std::byte{'I'}, std::byte{'D'},
                                                        std::byte{'M'}, std::byte{'P'}};
        storage::Page directory;
        std::copy(idmd_magic.begin(), idmd_magic.end(), directory.bytes().begin());
        store_le<std::uint16_t>(directory.bytes().subspan(4, 2), 1);
        store_le<std::uint64_t>(directory.bytes().subspan(16, 8), entries_id->value);
        storage::Page entries;
        std::copy(idmp_magic.begin(), idmp_magic.end(), entries.bytes().begin());
        store_le<std::uint16_t>(entries.bytes().subspan(4, 2), 1);
        constexpr std::size_t legacy_offset = 16 + first_user_object_id * 16;
        store_le<std::uint64_t>(entries.bytes().subspan(legacy_offset, 8), 77);
        store_le<std::uint16_t>(entries.bytes().subspan(legacy_offset + 8, 2), 3);
        store_le<std::uint16_t>(entries.bytes().subspan(legacy_offset + 10, 2), 9);
        store_le<std::uint32_t>(entries.bytes().subspan(legacy_offset + 12, 4), 1);
        suite.check(legacy_file->write(*directory_id, directory).has_value() &&
                        legacy_file->write(*entries_id, entries).has_value(),
                    "legacy v1 fixture is written");

        auto migrated = IdentityMap::open(*legacy_file, *directory_id);
        suite.check(migrated.has_value() && migrated->directory_root() != *directory_id,
                    "opening v1 creates a distinct v2 identity map");
        const RecordId expected{PageId{77}, SlotId{3}, 9};
        suite.check(migrated && migrated->find(ObjectId{first_user_object_id}) ==
                                    Result<RecordId>{expected},
                    "v1 binding survives the v2 migration");
        if (migrated) {
            suite.check(legacy_file->flush().has_value(), "migrated v2 map is flushed");
            auto reopened_map = IdentityMap::open(*legacy_file, migrated->directory_root());
            suite.check(reopened_map.has_value() && reopened_map->directory_root() ==
                                                    migrated->directory_root() &&
                            reopened_map->find(ObjectId{first_user_object_id}) ==
                                Result<RecordId>{expected},
                        "v2 map reopens without a second migration");
        }
    }

    return suite.finish();
}
