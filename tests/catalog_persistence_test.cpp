// Importa o ObjectStore (que orquestra o catálogo) e o PageFile.
#include <sstream>

#define private public
#include "modb/object/object_store.hpp"
#undef private
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


namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-catalog-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Um tipo com todos os recursos: default, coleção, embutido, owned e nullable.
TypeDefinition rich_type() {
    AttributeDefinition country{.id = FieldId{3}, .name = "country",
                              .type = AttributeType::string, .nullable = true};
    country.default_value = AttributeValue{"BR"};
    AttributeDefinition tags{.id = FieldId{4}, .name = "tags", .type = AttributeType::ref,
                           .nullable = true, .is_collection = true};
    AttributeDefinition address{.id = FieldId{5}, .name = "address", .type = AttributeType::ref,
                              .nullable = true, .is_embedded = true};
    AttributeDefinition manager{.id = FieldId{6}, .name = "manager", .type = AttributeType::ref,
                              .nullable = true, .is_owned = true};
    return *TypeDefinition::create(
        "Employee", std::vector<AttributeDefinition>{
                        AttributeDefinition{.id = FieldId{1}, .name = "name",
                                          .type = AttributeType::string, .nullable = false},
                        AttributeDefinition{.id = FieldId{2}, .name = "salary",
                                          .type = AttributeType::float64, .nullable = false},
                        country, tags, address, manager,
                    });
}

} // namespace

int main() {
    TestSuite suite;

    // --- save/load de tipo (com todos os recursos) e baseline ---
    {
        TemporaryDatabase database{"types"};
        TypeDefinitionId employee_id{};
        TypeDefinitionId latest_employee_id{};
        {
            auto file = PageFile::create(database.path());
            if (!file) {
                suite.check(false, "database created");
                return suite.finish();
            }
            auto store = ObjectStore::create(*file);
            if (!store) {
                suite.check(false, "store created");
                return suite.finish();
            }
            auto id = store->register_type(rich_type());
            suite.check(id.has_value(), "rich type registered");
            if (!id) {
                return suite.finish();
            }
            employee_id = *id;
            // O mesmo nome cria uma versão histórica nova.
            auto evolved = store->register_type(rich_type());
            suite.check(evolved.has_value() && evolved->value > employee_id.value,
                        "registering the same name creates a newer type version");
            if (evolved) {
                latest_employee_id = *evolved;
            }
            suite.check(store->current_baseline().has_value(),
                        "a baseline exists after registering a type");
            suite.check(file->flush().has_value(), "changes flushed");
        }

        {
            auto file = PageFile::open(database.path());
            if (!file) {
                suite.check(false, "database reopened");
                return suite.finish();
            }
            auto store = ObjectStore::open(*file);
            suite.check(store.has_value(), "store reopened");
            if (!store) {
                return suite.finish();
            }
            auto restored = store->find_type(employee_id);
            suite.check(restored.has_value(), "type restored by id after reopening");
            if (restored) {
                // A definição completa sobrevive: comparar contra o esperado
                // (com o id estampado) valida nome, atributos, defaults e flags.
                const auto& def = restored->get();
                suite.check(def.name() == "Employee", "restored type keeps its name");
                suite.check(def.attributes().size() == 6, "restored type keeps all attributes");
                const auto* country = def.find(FieldId{3});
                suite.check(country != nullptr && country->default_value.has_value() &&
                                country->default_value->as_string() ==
                                    Result<std::string_view>{"BR"},
                            "a default value survives the round-trip");
                const auto* tags = def.find(FieldId{4});
                suite.check(tags != nullptr && tags->is_collection,
                            "the collection flag survives the round-trip");
                const auto* address = def.find(FieldId{5});
                suite.check(address != nullptr && address->is_embedded,
                            "the embedded flag survives the round-trip");
                const auto* manager = def.find(FieldId{6});
                suite.check(manager != nullptr && manager->is_owned,
                            "the owned flag survives the round-trip");
            }
            // A baseline corrente foi restaurada e contém o tipo.
            suite.check(store->current_baseline().has_value() &&
                            store->current_baseline()->types().size() == 1,
                        "the current baseline is restored");
            // find por nome também funciona.
            auto latest = store->find_type("Employee");
            suite.check(latest.has_value() && latest->get().id() == latest_employee_id,
                        "type lookup by name restores the latest version");
        }
    }

    // --- abrir um arquivo relacional/sem DBRT dá erro claro, sem crash ---
    {
        TemporaryDatabase database{"legacy"};
        auto file = PageFile::create(database.path());
        suite.check(file.has_value(), "plain database created (no object store)");
        if (file) {
            // Um PageFile recém-criado não tem DBRT: ObjectStore::open recusa.
            suite.check_error(ObjectStore::open(*file), ErrorCode::invalid_file_format,
                              "opening a database without a DBRT root is rejected clearly");
        }
    }

    return suite.finish();
}
