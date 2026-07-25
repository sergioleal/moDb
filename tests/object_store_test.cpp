// Importa o ObjectStore exercitado neste teste.
#include <sstream>

#define private public
#include "modb/object/object_store.hpp"
#undef private
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
// Disponibiliza o mapa usado na verificação por conteúdo do scan.
#include <unordered_map>

using namespace modb;
using namespace modb::object;
using modb::storage::PageFile;


namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-store-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Cria o tipo Employee(name TEXT not null, salary REAL not null) num store.
Result<TypeDefinitionId> define_employee(ObjectStore& store) {
    auto type = TypeDefinition::create(
        "Employee", std::vector<AttributeDefinition>{
                        AttributeDefinition{.id = FieldId{1}, .name = "name",
                                          .type = AttributeType::string, .nullable = false},
                        AttributeDefinition{.id = FieldId{2}, .name = "salary",
                                          .type = AttributeType::float64, .nullable = false},
                    });
    if (!type) {
        return std::unexpected(type.error());
    }
    return store.register_type(std::move(*type));
}

// Monta o payload de um Employee.
FieldValues employee(std::string_view name, double salary) {
    return FieldValues{{FieldId{1}, AttributeValue{name}}, {FieldId{2}, AttributeValue{salary}}};
}

} // namespace

int main() {
    TestSuite suite;

    // --- create/get, ids monotônicos, update, remove, scan ---
    {
        TemporaryDatabase database{"crud"};
        auto file = PageFile::create(database.path());
        suite.check(file.has_value(), "database created");
        if (!file) {
            return suite.finish();
        }
        auto store = ObjectStore::create(*file);
        suite.check(store.has_value(), "object store created");
        if (!store) {
            return suite.finish();
        }

        auto type_id = define_employee(*store);
        suite.check(type_id.has_value(), "type registered");
        if (!type_id) {
            return suite.finish();
        }
        auto type = store->find_type(*type_id);
        suite.check(type.has_value(), "type found by id");
        if (!type) {
            return suite.finish();
        }

        suite.check_error(store->create_object(type->get(), employee("Ana", 15000.0)),
                          ErrorCode::transaction_required,
                          "object creation without a transaction is rejected");
        file->begin_transaction();
        auto ana = store->create_object(type->get(), employee("Ana", 15000.0));
        suite.check(ana.has_value() && ana->value >= first_user_object_id,
                    "object created with a user ObjectId");
        auto bia = store->create_object(type->get(), employee("Beatriz", 20000.0));
        suite.check(bia.has_value(), "second object created");
        suite.check(ana.has_value() && bia.has_value() && bia->value > ana->value,
                    "object ids are monotonically increasing");

        // get devolve o conteúdo idêntico.
        auto got = store->get(*ana);
        suite.check(got.has_value() && got->id == *ana && got->fields == employee("Ana", 15000.0),
                    "get returns the exact stored object");

        // update preserva a identidade.
        suite.check(store->update(*ana, type->get(), employee("Ana Maria", 16000.0), std::nullopt)
                        .has_value(),
                    "object updated");
        auto updated = store->get(*ana);
        suite.check(updated.has_value() && updated->id == *ana &&
                        updated->fields == employee("Ana Maria", 16000.0),
                    "update is visible and keeps the identity");

        // remove: get falha e o id não é reutilizado.
        suite.check(store->remove(*bia, std::nullopt).has_value(), "object removed");
        suite.check_error(store->get(*bia), ErrorCode::record_not_found,
                          "a removed object is not found");
        auto carol = store->create_object(type->get(), employee("Carol", 21000.0));
        suite.check(carol.has_value() && bia.has_value() && carol->value > bia->value,
                    "a removed id is never reused");

        // scan enumera exatamente os vivos (Ana Maria e Carol).
        std::size_t seen = 0;
        auto scanned = store->scan([&](const DecodedObject& object) -> Result<void> {
            ++seen;
            (void)object;
            return {};
        });
        suite.check(scanned.has_value() && seen == 2, "scan enumerates exactly the live objects");
        suite.check(file->apply_transaction().has_value(), "object transaction is applied");
        suite.check(file->flush().has_value(), "object transaction is flushed");
    }

    // --- critério de aceite da fase: 500 objetos, fechar, reabrir, verificar ---
    {
        TemporaryDatabase database{"reopen"};
        constexpr int total = 500;
        // Guarda o id do primeiro objeto de dados (não é derivável do piso: o
        // tipo e a baseline também consomem ids do mesmo contador).
        std::optional<ObjectId> first_object_id;

        {
            auto file = PageFile::create(database.path());
            suite.check(file.has_value(), "reopen: database created");
            if (!file) {
                return suite.finish();
            }
            auto store = ObjectStore::create(*file);
            if (!store) {
                suite.check(false, "reopen: store created");
                return suite.finish();
            }
            auto type_id = define_employee(*store);
            if (!type_id) {
                suite.check(false, "reopen: type registered");
                return suite.finish();
            }
            auto type = store->find_type(*type_id);
            bool all_created = type.has_value();
            file->begin_transaction();
            for (int i = 0; i < total && all_created; ++i) {
                // Nomes de comprimento variado exercitam múltiplas páginas.
                const std::string name = "employee-" + std::to_string(i) +
                                         std::string(static_cast<std::size_t>(i % 40), 'x');
                auto created = store->create_object(type->get(), employee(name, 1000.0 + i));
                if (!created) {
                    all_created = false;
                } else if (i == 0) {
                    first_object_id = *created;
                }
            }
            suite.check(all_created, "reopen: 500 objects created across many pages");
            suite.check(file->apply_transaction().has_value(), "reopen: object transaction is applied");
            suite.check(file->flush().has_value(), "reopen: changes flushed");
        }

        // Reabre a instância do zero.
        {
            auto file = PageFile::open(database.path());
            suite.check(file.has_value(), "reopen: database reopened");
            if (!file) {
                return suite.finish();
            }
            auto store = ObjectStore::open(*file);
            suite.check(store.has_value(), "reopen: object store reopened");
            if (!store) {
                return suite.finish();
            }
            // O tipo foi restaurado do catálogo.
            auto type = store->find_type("Employee");
            suite.check(type.has_value(), "reopen: type restored from catalog");

            // Todos os 500 recuperados por scan. A ordem física do scan não é a
            // ordem de inserção (registros de tamanho variável reaproveitam
            // buracos em páginas anteriores), então a verificação é por
            // conteúdo, não por posição: coleta nome->salário e confere o
            // conjunto esperado.
            std::unordered_map<std::string, double> found;
            auto scanned = store->scan([&](const DecodedObject& object) -> Result<void> {
                auto name = object.fields.at(0).second.as_string();
                auto salary = object.fields.at(1).second.as_float64();
                if (name && salary) {
                    found.emplace(std::string{*name}, *salary);
                }
                return {};
            });
            suite.check(scanned.has_value() && found.size() == total,
                        "reopen: all 500 objects recovered by scan");
            bool all_correct = true;
            for (int i = 0; i < total; ++i) {
                const std::string name = "employee-" + std::to_string(i) +
                                         std::string(static_cast<std::size_t>(i % 40), 'x');
                const auto it = found.find(name);
                if (it == found.end() || it->second != 1000.0 + static_cast<double>(i)) {
                    all_correct = false;
                    break;
                }
            }
            suite.check(all_correct, "reopen: every stored object is recovered with its content");

            // E recuperáveis por get, pelo id capturado na criação.
            suite.check(first_object_id.has_value(), "reopen: first object id captured");
            if (first_object_id) {
                auto first = store->get(*first_object_id);
                suite.check(first.has_value() &&
                                first->fields == employee("employee-0", 1000.0),
                            "reopen: an object is retrievable by id");
            }
        }
    }

    // --- transação exigida em update/remove/create_index; tipo não registrado ---
    {
        TemporaryDatabase database{"guards"};
        auto file = PageFile::create(database.path());
        suite.check(file.has_value(), "guards: database created");
        if (!file) {
            return suite.finish();
        }
        auto store = ObjectStore::create(*file);
        suite.check(store.has_value(), "guards: object store created");
        if (!store) {
            return suite.finish();
        }
        auto type_id = define_employee(*store);
        suite.check(type_id.has_value(), "guards: type registered");
        if (!type_id) {
            return suite.finish();
        }
        auto type = store->find_type(*type_id);
        if (!type) {
            suite.check(false, "guards: type found");
            return suite.finish();
        }

        // Um objeto de um tipo NUNCA registrado neste store: id() fica
        // invalid_object_id, então create_object rejeita antes de tocar o heap.
        auto unregistered = TypeDefinition::create(
            "Ghost", std::vector<AttributeDefinition>{AttributeDefinition{
                        .id = FieldId{1}, .name = "x", .type = AttributeType::int64, .nullable = false}});
        suite.check(unregistered.has_value(), "guards: unregistered type builds");
        if (unregistered) {
            file->begin_transaction();
            suite.check_error(
                store->create_object(*unregistered, FieldValues{{FieldId{1}, AttributeValue{std::int64_t{1}}}}),
                ErrorCode::type_not_found,
                "guards: creating an object of an unregistered type is rejected");
            static_cast<void>(file->apply_transaction());
        }

        file->begin_transaction();
        auto dave = store->create_object(type->get(), employee("Dave", 12000.0));
        suite.check(dave.has_value(), "guards: object created for guard checks");
        static_cast<void>(file->apply_transaction());
        static_cast<void>(file->flush());

        if (dave) {
            suite.check_error(store->update(*dave, type->get(), employee("Dave", 13000.0), std::nullopt),
                              ErrorCode::transaction_required,
                              "guards: update outside a transaction is rejected");
            suite.check_error(store->remove(*dave, std::nullopt), ErrorCode::transaction_required,
                              "guards: remove outside a transaction is rejected");
        }
        suite.check_error(store->create_index(type->get(), FieldId{2}), ErrorCode::transaction_required,
                          "guards: create_index outside a transaction is rejected");

        // Sem nenhum índice criado ainda: index_equal/index_range falham com
        // "no index on this field" (indexes_ ainda é nullopt).
        suite.check_error(store->index_equal("Employee", 2, AttributeValue{12000.0}),
                          ErrorCode::type_not_found,
                          "guards: index_equal with no indexes at all is rejected");
    }

    // --- índices: create_index, has_index, index_equal/index_range, e
    // manutenção incremental via update/remove (Fase 7B) ---
    {
        TemporaryDatabase database{"index"};
        auto file = PageFile::create(database.path());
        auto store = file ? ObjectStore::create(*file)
                          : Result<ObjectStore>{std::unexpected(Error{ErrorCode::io_error, "no file"})};
        suite.check(file.has_value() && store.has_value(), "index: object store created");
        if (!file || !store) {
            return suite.finish();
        }
        auto type_id = define_employee(*store);
        auto type = type_id ? store->find_type(*type_id)
                            : Result<std::reference_wrapper<const TypeDefinition>>{
                                  std::unexpected(Error{ErrorCode::type_not_found, "no type"})};
        suite.check(type.has_value(), "index: type registered");
        if (!type) {
            return suite.finish();
        }

        file->begin_transaction();
        // create_index rejeita um campo que não existe no tipo.
        suite.check_error(store->create_index(type->get(), FieldId{99}), ErrorCode::invalid_argument,
                          "index: create_index rejects a field the type doesn't have");
        suite.check(store->create_index(type->get(), FieldId{2}).has_value(),
                    "index: index on salary created");
        suite.check(store->has_index("Employee", 2), "index: has_index reports the new index");
        suite.check(!store->has_index("Employee", 1), "index: has_index is false for an unindexed field");

        auto ana = store->create_object(type->get(), employee("Ana", 15000.0));
        auto bia = store->create_object(type->get(), employee("Beatriz", 20000.0));
        auto cid = store->create_object(type->get(), employee("Carol", 15000.0));  // mesmo salário de Ana
        suite.check(ana.has_value() && bia.has_value() && cid.has_value(),
                    "index: objects created after the index exists (index_maintain insert path)");
        static_cast<void>(file->apply_transaction());
        static_cast<void>(file->flush());

        if (ana && bia && cid) {
            auto equal_15000 = store->index_equal("Employee", 2, AttributeValue{15000.0});
            suite.check(equal_15000.has_value() && equal_15000->size() == 2,
                        "index: index_equal finds both objects sharing a salary");

            auto ranged = store->index_range("Employee", 2, AttributeValue{14000.0}, AttributeValue{16000.0});
            suite.check(ranged.has_value() && ranged->size() == 2,
                        "index: index_range finds salaries within the window");
            auto out_of_range =
                store->index_range("Employee", 2, AttributeValue{1.0}, AttributeValue{2.0});
            suite.check(out_of_range.has_value() && out_of_range->empty(),
                        "index: index_range is empty outside the window");

            // Campo sem índice: "no index on this field" com indexes_ já existente.
            suite.check_error(store->index_equal("Employee", 1, AttributeValue{"Ana"}),
                              ErrorCode::type_not_found,
                              "index: index_equal on an unindexed field is rejected");

            // update muda o salário de Ana: a chave antiga sai do índice, a
            // nova entra (index_maintain remove+insert).
            file->begin_transaction();
            suite.check(store->update(*ana, type->get(), employee("Ana", 30000.0), std::nullopt)
                            .has_value(),
                        "index: salary updated");
            static_cast<void>(file->apply_transaction());
            static_cast<void>(file->flush());

            auto after_update = store->index_equal("Employee", 2, AttributeValue{15000.0});
            suite.check(after_update.has_value() && after_update->size() == 1 &&
                            after_update->front() == *cid,
                        "index: only Carol remains at the old salary after Ana's update");
            auto moved = store->index_equal("Employee", 2, AttributeValue{30000.0});
            suite.check(moved.has_value() && moved->size() == 1 && moved->front() == *ana,
                        "index: Ana is found at her new salary");

            // remove tira Carol do índice (index_maintain remove path).
            file->begin_transaction();
            suite.check(store->remove(*cid, std::nullopt).has_value(), "index: Carol removed");
            static_cast<void>(file->apply_transaction());
            static_cast<void>(file->flush());

            auto after_remove = store->index_equal("Employee", 2, AttributeValue{15000.0});
            suite.check(after_remove.has_value() && after_remove->empty(),
                        "index: no one is left at the old salary after removal");
        }
    }

    return suite.finish();
}
