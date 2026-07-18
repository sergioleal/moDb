// Valida os índices da Fase 7B ponta a ponta: criação com backfill, consulta
// por igualdade e faixa via B+ tree (em ordem), manutenção transacional em
// create/update/remove, duplicatas, uso comprovado do índice (menos páginas que
// um scan completo), erro sem índice e sobrevivência a recovery e reabertura.
#include "modb/object/database.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

using namespace modb;
using namespace modb::object;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-index-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        auto wal = path_;
        wal += ".wal";
        std::filesystem::remove(wal, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

struct Person {
    std::string name;
    std::int64_t age{};
    friend bool operator==(const Person&, const Person&) = default;
};

// FieldIds: name = 1, age = 2.
BindingBuilder<Person> person_builder() {
    BindingBuilder<Person> builder{"Person"};
    builder.field<1>("name", &Person::name).field<2>("age", &Person::age);
    return builder;
}

std::shared_ptr<Database> share(Result<Database>& result) {
    if (!result) {
        return {};
    }
    return std::make_shared<Database>(std::move(*result));
}

Result<ObjectId> create_committed(Database& database, Person value) {
    auto tx = database.begin();
    if (!tx) {
        return std::unexpected(tx.error());
    }
    auto handle = database.create(*tx, value);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    if (auto committed = tx->commit(); !committed) {
        return std::unexpected(committed.error());
    }
    return handle->id();
}

Result<void> set_age(Database& database, ObjectId id, std::int64_t age) {
    auto tx = database.begin();
    if (!tx) {
        return std::unexpected(tx.error());
    }
    auto handle = database.get<Person>(id);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    if (auto set = handle->set<&Person::age>(*tx, age); !set) {
        return std::unexpected(set.error());
    }
    return tx->commit();
}

// Coleta as idades de um stream de Person.
std::vector<std::int64_t> ages_of(query::Generator<Result<Person>> stream) {
    std::vector<std::int64_t> out;
    for (auto& result : stream) {
        if (result) {
            out.push_back(result->age);
        }
    }
    return out;
}

} // namespace

int main() {
    TestSuite suite;
    TemporaryDatabase temp{"main"};
    ObjectId target{};

    {
        auto created = Database::create(temp.path());
        auto database = share(created);
        suite.check(database != nullptr, "index database is created");
        if (!database) {
            return suite.finish();
        }
        auto database_id = DatabaseRegistry::instance().attach(database);
        suite.check(database_id.has_value(), "database is attached");
        suite.check(database->bind(person_builder()).has_value(), "Person is bound");

        // Muitas pessoas em várias páginas, idades 0..N-1 e algumas repetidas.
        constexpr int total = 400;
        for (int i = 0; i < total; ++i) {
            auto id = create_committed(*database, Person{"p" + std::to_string(i), i % 100});
            if (i == 250) {
                if (id) {
                    target = *id;  // age 50, para exercitar update/remove
                }
            }
        }

        // Antes do índice: equals falha (não há índice sobre o campo) — o erro
        // chega como o primeiro item do stream.
        {
            bool saw_expected = false;
            for (auto& result :
                 database->query<Person>().equals(FieldId{2}, std::int64_t{50}).stream()) {
                saw_expected = !result && result.error().code == ErrorCode::type_not_found;
                break;
            }
            suite.check(saw_expected, "querying an unindexed field yields type_not_found");
        }

        // Cria o índice sobre `age` (backfill dos 400).
        suite.check(database->create_index<Person>(FieldId{2}).has_value(),
                    "an index on age is created and backfilled");
        // Índice duplicado é rejeitado.
        suite.check_error(database->create_index<Person>(FieldId{2}), ErrorCode::invalid_argument,
                          "creating the same index twice is rejected");

        // Igualdade: age == 50 casa com quem tem i%100==50 -> 4 pessoas (50,150,250,350).
        {
            auto matches = ages_of(database->query<Person>().equals(FieldId{2}, std::int64_t{50})
                                       .stream());
            suite.check(matches.size() == 4, "equality returns every matching object (duplicates)");
            bool all_50 = true;
            for (auto age : matches) {
                all_50 = all_50 && age == 50;
            }
            suite.check(all_50, "every equality hit has the queried value");
        }

        // Índice usado: uma busca seletiva lê bem menos páginas que o scan cheio.
        std::uint64_t full_pages = 0;
        {
            database->reset_data_pages_read();
            auto all = ages_of(database->query<Person>().stream());
            full_pages = database->data_pages_read();
            suite.check(all.size() == static_cast<std::size_t>(total) && full_pages > 2,
                        "a full scan reads many data pages");
        }
        {
            database->reset_data_pages_read();
            auto few = ages_of(database->query<Person>().equals(FieldId{2}, std::int64_t{50})
                                   .stream());
            const auto index_pages = database->data_pages_read();
            suite.check(few.size() == 4 && index_pages < full_pages,
                        "the index scan reads fewer data pages than a full scan");
        }

        // Faixa [10, 12] inclusive -> quem tem age em {10,11,12}, em ordem de age.
        {
            auto ranged = ages_of(database->query<Person>()
                                      .between(FieldId{2}, std::int64_t{10}, std::int64_t{12})
                                      .stream());
            bool ordered = true;
            for (std::size_t i = 1; i < ranged.size(); ++i) {
                ordered = ordered && ranged[i - 1] <= ranged[i];
            }
            // 4 por idade * 3 idades = 12.
            suite.check(ranged.size() == 12 && ordered,
                        "a range scan returns every hit in ascending order");
        }

        // Manutenção no UPDATE: mudar a idade do alvo (50 -> 777) reflete no índice.
        suite.check(set_age(*database, target, 777).has_value(), "target's age is updated");
        {
            auto old_hits = ages_of(database->query<Person>().equals(FieldId{2}, std::int64_t{50})
                                        .stream());
            auto new_hits = ages_of(database->query<Person>().equals(FieldId{2}, std::int64_t{777})
                                        .stream());
            suite.check(old_hits.size() == 3, "the updated object left its old index bucket");
            suite.check(new_hits.size() == 1 && new_hits.front() == 777,
                        "the updated object appears under its new value");
        }

        // Manutenção no REMOVE: remover o alvo tira-o do índice.
        {
            auto tx = database->begin();
            suite.check(tx.has_value() && database->remove(*tx, target).has_value() &&
                            tx->commit().has_value(),
                        "target is removed");
        }
        {
            auto gone = ages_of(database->query<Person>().equals(FieldId{2}, std::int64_t{777})
                                    .stream());
            suite.check(gone.empty(), "a removed object is gone from the index");
        }

        DatabaseRegistry::instance().detach(*database_id);
    }

    // Reabertura: o índice persiste e continua correto.
    {
        auto opened = Database::open(temp.path());
        auto database = share(opened);
        suite.check(database != nullptr, "database reopens");
        if (!database) {
            return suite.finish();
        }
        auto database_id = DatabaseRegistry::instance().attach(database);
        suite.check(database->bind(person_builder()).has_value(), "Person is rebound");
        auto hits = ages_of(database->query<Person>().equals(FieldId{2}, std::int64_t{50})
                                .stream());
        suite.check(hits.size() == 3,
                    "the index survives closing and reopening (still 3 after the earlier update)");
        auto ranged = ages_of(database->query<Person>()
                                  .between(FieldId{2}, std::int64_t{0}, std::int64_t{2})
                                  .stream());
        suite.check(ranged.size() == 12, "range queries work after reopening");
        DatabaseRegistry::instance().detach(*database_id);
    }

    // Recovery: um create com índice, commit durável mas não aplicado, é refeito
    // por completo (objeto E índice) na reabertura.
    {
        TemporaryDatabase rec{"recovery"};
        {
            auto created = Database::create(rec.path());
            auto database = share(created);
            suite.check(database != nullptr, "recovery database created");
            if (!database) {
                return suite.finish();
            }
            auto database_id = DatabaseRegistry::instance().attach(database);
            suite.check(database->bind(person_builder()).has_value(), "Person bound");
            suite.check(create_committed(*database, Person{"seed", 1}).has_value(), "seed committed");
            suite.check(database->create_index<Person>(FieldId{2}).has_value(), "index created");
            // Novo objeto com commit durável no WAL, mas queda antes de aplicar.
            auto tx = database->begin();
            suite.check(tx.has_value(), "recovery transaction begins");
            if (tx) {
                suite.check(database->create(*tx, Person{"late", 99}).has_value(), "late staged");
                suite.check(tx->commit(CommitPhase::stop_after_commit_record).has_value(),
                            "commit record is durable before applying");
            }
            DatabaseRegistry::instance().detach(*database_id);  // abandona: queda simulada
        }
        auto opened = Database::open(rec.path());
        auto database = share(opened);
        suite.check(database != nullptr, "recovery database reopens");
        if (!database) {
            return suite.finish();
        }
        auto database_id = DatabaseRegistry::instance().attach(database);
        suite.check(database->bind(person_builder()).has_value(), "Person rebound after recovery");
        auto hits = ages_of(database->query<Person>().equals(FieldId{2}, std::int64_t{99})
                                .stream());
        suite.check(hits.size() == 1 && hits.front() == 99,
                    "recovery reapplied both the object and its index entry");
        DatabaseRegistry::instance().detach(*database_id);
    }

    return suite.finish();
}
