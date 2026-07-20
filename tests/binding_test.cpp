// Importa o Database tipado e o Binding exercitados neste teste.
#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
#include "modb/storage/endian.hpp"
#include "modb/storage/page_file.hpp"

// Importa as funções simples de verificação dos testes.
#include "test_support.hpp"

// Disponibiliza o relógio usado no nome único do arquivo.
#include <chrono>
// Disponibiliza caminhos temporários.
#include <filesystem>
// Disponibiliza shared_ptr para registrar o Database.
#include <memory>
// Disponibiliza std::error_code na limpeza.
#include <system_error>

using namespace modb;
using namespace modb::object;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-binding-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Uma classe C++ de domínio, com os quatro tipos escalares que o binding cobre.
struct Employee {
    std::string name;
    double salary{};
    bool active{};
    std::int64_t level{};

    friend bool operator==(const Employee&, const Employee&) = default;
};

// Monta o builder do Employee (reutilizado em várias verificações).
BindingBuilder<Employee> employee_builder() {
    BindingBuilder<Employee> builder{"Employee"};
    builder.field<1>("name", &Employee::name)
        .field<2>("salary", &Employee::salary)
        .field<3>("active", &Employee::active)
        .field<4>("level", &Employee::level);
    return builder;
}

} // namespace

int main() {
    TestSuite suite;

    // --- validação do builder ---
    {
        auto binding = employee_builder().build();
        suite.check(binding.has_value(), "a well-formed binding builds");
        if (binding) {
            auto type = binding->to_type_definition();
            suite.check(type.has_value() && type->name() == "Employee" &&
                            type->attributes().size() == 4,
                        "the canonical type definition matches the binding");
            if (type) {
                const auto* salary = type->find(FieldId{2});
                suite.check(salary != nullptr && salary->type == AttributeType::float64 &&
                                !salary->nullable,
                            "a bound field maps to the right attribute type, non-nullable");
            }
        }

        BindingBuilder<Employee> dup_id{"Bad"};
        dup_id.field<1>("a", &Employee::name).field<1>("b", &Employee::salary);
        suite.check_error(dup_id.build(), ErrorCode::duplicate_field,
                          "a duplicate FieldId is rejected");

        BindingBuilder<Employee> zero{"Bad"};
        zero.field<0>("a", &Employee::name);
        suite.check_error(zero.build(), ErrorCode::invalid_argument, "FieldId zero is rejected");

        BindingBuilder<Employee> dup_name{"Bad"};
        dup_name.field<1>("same", &Employee::name).field<2>("same", &Employee::salary);
        suite.check_error(dup_name.build(), ErrorCode::duplicate_column,
                          "a duplicate field name is rejected");
    }

    const Employee ana{"Ana", 15000.0, true, 3};

    // --- round-trip: cria um Employee real, persiste, materializa de volta ---
    TemporaryDatabase database{"roundtrip"};
    ObjectId ana_id{};
    BaselineId original_baseline{};
    std::uint64_t committed_epoch{};
    {
        auto created = Database::create(database.path());
        suite.check(created.has_value(), "database is created");
        if (!created) {
            return suite.finish();
        }
        auto db = std::make_shared<Database>(std::move(*created));
        auto database_id = DatabaseRegistry::instance().attach(db);
        suite.check(database_id.has_value(), "database is attached");
        if (!database_id) {
            return suite.finish();
        }
        suite.check(db->epoch() == 0, "a new database starts at epoch zero");
        // Criar sem bind é rejeitado (dentro de uma transação, que é exigida).
        {
            auto unbound_tx = db->begin();
            suite.check(unbound_tx.has_value(), "a transaction begins");
            if (unbound_tx) {
                suite.check_error(db->bind(employee_builder()), ErrorCode::transaction_active,
                                  "binding during a transaction is rejected");
                suite.check_error(db->create(*unbound_tx, ana), ErrorCode::type_not_found,
                                  "rejected binding does not leave the type bound");
                (void)unbound_tx->rollback();
            }
        }

        // O schema dinÃ¢mico tambÃ©m usa a transaÃ§Ã£o do Database: este Ã© o
        // caminho que alimenta `modb type define`, sem acesso gravÃ¡vel ao
        // ObjectStore nem flush direto do catÃ¡logo.
        auto dynamic_type = TypeDefinition::create(
            "CliDefined", std::vector<AttributeDefinition>{
                              AttributeDefinition{.id = FieldId{1}, .name = "value",
                                                  .type = AttributeType::string, .nullable = false},
                          });
        suite.check(dynamic_type.has_value(), "a dynamic type definition builds");
        auto dynamic_tx = db->begin();
        suite.check(dynamic_tx.has_value(), "a transaction begins for dynamic schema");
        if (dynamic_type && dynamic_tx) {
            auto dynamic_id = db->define_type(*dynamic_tx, std::move(*dynamic_type));
            suite.check(dynamic_id.has_value(),
                        "dynamic schema definition is accepted only through Database transaction");
            suite.check(dynamic_tx->commit().has_value(), "dynamic schema transaction is committed");
        }

        suite.check(db->bind(employee_builder()).has_value(), "the Employee type is bound");
        suite.check(db->epoch() == 2,
                    "each committed catalog transaction advances the global epoch");
        if (db->current_baseline()) {
            original_baseline = db->current_baseline()->id();
        }
        auto transaction = db->begin();
        suite.check(transaction.has_value(), "the write transaction begins");
        if (!transaction) {
            return suite.finish();
        }
        auto handle = db->create(*transaction, ana);
        suite.check(handle.has_value(), "a real Employee object is persisted");
        if (!handle) {
            return suite.finish();
        }
        ana_id = handle->id();

        auto materialized = db->materialize(*handle);
        suite.check(materialized.has_value() && *materialized == ana,
                    "the materialized object equals the original C++ object");

        suite.check(transaction->commit().has_value(), "changes are committed");
        committed_epoch = db->epoch();
        suite.check(committed_epoch == 3, "object commit advances the global epoch once");
        auto wal_path = database.path();
        wal_path += ".wal";
        suite.check(std::filesystem::exists(wal_path),
                    "a full commit keeps the durable WAL");
        DatabaseRegistry::instance().detach(*database_id);
    }

    // --- reabertura: rebind idêntico adota o tipo; materializa o objeto salvo ---
    {
        auto opened = Database::open(database.path());
        suite.check(opened.has_value(), "database is reopened");
        if (!opened) {
            return suite.finish();
        }
        auto db = std::make_shared<Database>(std::move(*opened));
        auto database_id = DatabaseRegistry::instance().attach(db);
        if (!database_id) {
            suite.check(false, "reopened database is attached");
            return suite.finish();
        }
        // O mesmo binding, na reabertura, adota o tipo persistido (não falha e
        // não cria uma nova definição).
        suite.check(db->epoch() == committed_epoch,
                    "the global epoch is preserved when reopening the database");
        suite.check(db->bind(employee_builder()).has_value(),
                    "rebinding the identical type adopts the persisted definition");
        suite.check(db->current_baseline().has_value() &&
                        db->current_baseline()->id() == original_baseline,
                    "an identical binding does not create a type or baseline");

        auto handle = db->get<Employee>(ana_id);
        suite.check(handle.has_value(), "the persisted object is found by id");
        if (handle) {
            auto materialized = db->materialize(*handle);
            suite.check(materialized.has_value() && *materialized == ana,
                        "the object survives closing and reopening as a C++ object");
        }
        DatabaseRegistry::instance().detach(*database_id);
    }

    // DBRT v1 é atualizado para v2 na abertura, inicializando a época em zero.
    {
        TemporaryDatabase legacy_root{"dbrt-v1"};
        {
            auto created = Database::create(legacy_root.path());
            suite.check(created.has_value(), "legacy DBRT fixture database is created");
            if (!created) {
                return suite.finish();
            }
        }
        {
        auto file = storage::PageFile::open(legacy_root.path());
        suite.check(file.has_value() && file->catalog_root().has_value(),
                    "legacy DBRT fixture is opened");
        if (!file || !file->catalog_root()) {
            return suite.finish();
        }
        auto root_page = file->read(*file->catalog_root());
        if (!root_page) {
            suite.check(false, "DBRT fixture page is read");
            return suite.finish();
        }
        storage::store_le<std::uint16_t>(root_page->bytes().subspan(4, 2), 1);
        suite.check(file->write(*file->catalog_root(), *root_page).has_value() &&
                        file->flush().has_value(),
                    "DBRT v1 fixture is persisted");
        }

        {
        auto upgraded = Database::open(legacy_root.path());
        suite.check(upgraded.has_value() && upgraded->epoch() == 0,
                    "DBRT v1 opens as v2 with epoch zero");
        }

        auto verified = storage::PageFile::open(legacy_root.path());
        auto verified_page =
            verified && verified->catalog_root() ? verified->read(*verified->catalog_root())
                                                  : Result<storage::Page>{std::unexpected(verified.error())};
        suite.check(verified_page.has_value() &&
                        storage::load_le<std::uint16_t>(verified_page->bytes().subspan(4, 2)) == 2,
                    "DBRT v1 upgrade is persisted as version two");
    }

    return suite.finish();
}
