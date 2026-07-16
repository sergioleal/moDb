// Importa o mapa de identidade exercitado neste teste.
#include "modb/object/identity_map.hpp"
// Importa PageFile para criar/reabrir o arquivo.
#include "modb/storage/page_file.hpp"

// Importa as funções simples de verificação dos testes.
#include "test_support.hpp"

// Disponibiliza o relógio usado no nome único do arquivo.
#include <chrono>
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
        suite.check(map->bind(ObjectId{16}, record).has_value(), "an object binds");
        suite.check(map->find(ObjectId{16}) == Result<RecordId>{record},
                    "a bound object is found at the same location");

        // find de id inexistente.
        suite.check_error(map->find(ObjectId{17}), ErrorCode::record_not_found,
                          "an unbound id is reported as not found");

        // rebind: atualiza a localização.
        const auto moved = make_record(99);
        suite.check(map->rebind(ObjectId{16}, moved).has_value(), "a bound object rebinds");
        suite.check(map->find(ObjectId{16}) == Result<RecordId>{moved},
                    "a rebound object is found at the new location");

        // erase: find falha, rebind falha.
        suite.check(map->erase(ObjectId{16}).has_value(), "a bound object is erased");
        suite.check_error(map->find(ObjectId{16}), ErrorCode::record_not_found,
                          "an erased object is not found");
        suite.check_error(map->rebind(ObjectId{16}, record), ErrorCode::record_not_found,
                          "an erased object cannot be rebound");

        // Um id nunca reutilizado permanece com tombstone: novo bind do MESMO id
        // é rejeitado (o motor nunca reusa ids, mas a guarda precisa existir).
        suite.check_error(map->bind(ObjectId{16}, record), ErrorCode::invalid_argument,
                          "an erased id cannot be bound again");

        // Crescimento: muitos binds forçam várias páginas IDMP.
        bool all_found = true;
        for (std::uint64_t id = 100; id < 10100; ++id) {
            if (!map->bind(ObjectId{id}, make_record(id))) {
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
        suite.check(map->bind(ObjectId{high_id}, make_record(high_id)).has_value(),
                    "a very high id binds, chaining a second directory");
        suite.check(map->find(ObjectId{high_id}) == Result<RecordId>{make_record(high_id)},
                    "the high id is found through the chained directory");

        suite.check(file.flush().has_value(), "map changes are flushed");
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

    return suite.finish();
}
