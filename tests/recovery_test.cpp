#include "modb/object/database.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

using namespace modb;
using namespace modb::object;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-recovery-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }

    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(wal_path(), ignored);
        std::filesystem::remove(wal_copy_path(), ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] std::filesystem::path wal_path() const {
        auto result = path_;
        result += ".wal";
        return result;
    }

    [[nodiscard]] std::filesystem::path wal_copy_path() const {
        auto result = path_;
        result += ".wal.copy";
        return result;
    }

private:
    std::filesystem::path path_;
};

struct Employee {
    std::string name;
    std::int64_t level{};

    friend bool operator==(const Employee&, const Employee&) = default;
};

BindingBuilder<Employee> employee_builder() {
    BindingBuilder<Employee> builder{"Employee"};
    builder.field<1>("name", &Employee::name).field<2>("level", &Employee::level);
    return builder;
}

std::shared_ptr<Database> share(Result<Database>& result) {
    if (!result) {
        return {};
    }
    return std::make_shared<Database>(std::move(*result));
}

Result<DatabaseId> attach(const std::shared_ptr<Database>& database) {
    return DatabaseRegistry::instance().attach(database);
}

void detach(const Result<DatabaseId>& id) {
    if (id) {
        DatabaseRegistry::instance().detach(*id);
    }
}

} // namespace

int main() {
    TestSuite suite;

    // Uma transação com commit durável, mas sem páginas aplicadas, é refeita
    // integralmente na abertura.
    {
        TemporaryDatabase temporary{"committed-redo"};
        ObjectId employee_id{};
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            suite.check(database != nullptr, "redo database is created");
            if (!database) {
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database_id.has_value(), "redo database is attached");
            suite.check(database->bind(employee_builder()).has_value(), "redo type is bound");

            auto transaction = database->begin();
            suite.check(transaction.has_value(), "redo transaction begins");
            if (!transaction) {
                detach(database_id);
                return suite.finish();
            }
            auto employee = database->create(*transaction, Employee{"Ana", 7});
            suite.check(employee.has_value(), "redo object is staged");
            if (employee) {
                employee_id = employee->id();
            }
            suite.check(transaction->commit(CommitPhase::stop_after_commit_record).has_value(),
                        "redo commit record is durable before applying pages");
            suite.check(std::filesystem::exists(temporary.wal_path()),
                        "redo WAL remains for recovery");
            detach(database_id);
        }

        auto opened = Database::open(temporary.path());
        auto database = share(opened);
        suite.check(database != nullptr, "redo database reopens");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database_id.has_value(), "recovered database is attached");
        suite.check(database->bind(employee_builder()).has_value(), "recovered type is rebound");
        auto handle = database->get<Employee>(employee_id);
        suite.check(handle.has_value(), "committed object is visible after redo");
        if (handle) {
            auto employee = database->materialize(*handle);
            suite.check(employee.has_value() && *employee == Employee{"Ana", 7},
                        "redo restores the complete object");
        }
        suite.check(!std::filesystem::exists(temporary.wal_path()),
                    "successful recovery removes the WAL");
        detach(database_id);
    }

    // Imagens sem registro de commit são ignoradas.
    {
        TemporaryDatabase temporary{"uncommitted"};
        ObjectId employee_id{};
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            if (!database) {
                suite.check(false, "uncommitted database is created");
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(employee_builder()).has_value(),
                        "uncommitted type is bound");
            auto transaction = database->begin();
            if (!transaction) {
                suite.check(false, "uncommitted transaction begins");
                detach(database_id);
                return suite.finish();
            }
            auto employee = database->create(*transaction, Employee{"Bia", 3});
            suite.check(employee.has_value(), "uncommitted object is staged");
            if (employee) {
                employee_id = employee->id();
            }
            suite.check(transaction->commit(CommitPhase::stop_after_images).has_value(),
                        "crash is simulated before the commit record");
            detach(database_id);
        }

        auto opened = Database::open(temporary.path());
        auto database = share(opened);
        suite.check(database != nullptr, "uncommitted database reopens");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(employee_builder()).has_value(),
                    "uncommitted database type is rebound");
        suite.check_error(database->get<Employee>(employee_id), ErrorCode::record_not_found,
                          "transaction without commit remains absent");
        suite.check(!std::filesystem::exists(temporary.wal_path()),
                    "uncommitted WAL is discarded");
        detach(database_id);
    }

    // Reaplicar o mesmo WAL duas vezes produz exatamente o mesmo estado.
    {
        TemporaryDatabase temporary{"idempotent"};
        ObjectId employee_id{};
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            if (!database) {
                suite.check(false, "idempotent database is created");
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(employee_builder()).has_value(), "idempotent type is bound");
            auto transaction = database->begin();
            if (!transaction) {
                suite.check(false, "idempotent transaction begins");
                detach(database_id);
                return suite.finish();
            }
            auto employee = database->create(*transaction, Employee{"Caio", 11});
            if (employee) {
                employee_id = employee->id();
            }
            suite.check(employee.has_value(), "idempotent object is staged");
            suite.check(transaction->commit(CommitPhase::stop_after_commit_record).has_value(),
                        "idempotent WAL is committed");
            std::error_code copy_error;
            std::filesystem::copy_file(temporary.wal_path(), temporary.wal_copy_path(),
                                       std::filesystem::copy_options::overwrite_existing,
                                       copy_error);
            suite.check(!copy_error, "committed WAL is copied for a second recovery");
            detach(database_id);
        }

        {
            auto opened = Database::open(temporary.path());
            auto database = share(opened);
            suite.check(database != nullptr, "first idempotent recovery succeeds");
            if (!database) {
                return suite.finish();
            }
        }

        std::error_code restore_error;
        std::filesystem::copy_file(temporary.wal_copy_path(), temporary.wal_path(),
                                   std::filesystem::copy_options::overwrite_existing,
                                   restore_error);
        suite.check(!restore_error, "same WAL is restored for repeated recovery");

        auto reopened = Database::open(temporary.path());
        auto database = share(reopened);
        suite.check(database != nullptr, "second idempotent recovery succeeds");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(employee_builder()).has_value(),
                    "idempotently recovered type is rebound");
        auto handle = database->get<Employee>(employee_id);
        suite.check(handle.has_value(), "object exists exactly once after repeated recovery");
        if (handle) {
            auto employee = database->materialize(*handle);
            suite.check(employee.has_value() && *employee == Employee{"Caio", 11},
                        "repeated recovery preserves object contents");
        }
        detach(database_id);
    }

    // Rollback explícito e destrutor de Transaction preservam dados anteriores.
    {
        TemporaryDatabase temporary{"rollback"};
        ObjectId employee_id{};
        ObjectId rolled_back_id{};
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            if (!database) {
                suite.check(false, "rollback database is created");
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(employee_builder()).has_value(), "rollback type is bound");

            auto seed = database->begin();
            if (!seed) {
                suite.check(false, "seed transaction begins");
                detach(database_id);
                return suite.finish();
            }
            auto employee = database->create(*seed, Employee{"Dora", 5});
            if (employee) {
                employee_id = employee->id();
            }
            suite.check(employee.has_value() && seed->commit().has_value(),
                        "pre-existing object is committed");

            auto rollback = database->begin();
            if (!rollback) {
                suite.check(false, "explicit rollback transaction begins");
                detach(database_id);
                return suite.finish();
            }
            auto transient = database->create(*rollback, Employee{"Eva", 99});
            if (transient) {
                rolled_back_id = transient->id();
            }
            suite.check(transient.has_value(), "transient object is staged");
            if (employee) {
                suite.check(database->update(*rollback, *employee, Employee{"Dora", 42}).has_value(),
                            "update is staged before explicit rollback");
            }
            suite.check(rollback->rollback().has_value(), "explicit rollback succeeds");
            suite.check_error(database->get<Employee>(rolled_back_id), ErrorCode::record_not_found,
                              "explicit rollback removes the transient object");

            {
                auto automatic = database->begin();
                suite.check(automatic.has_value(), "automatic rollback transaction begins");
                if (automatic && employee) {
                    suite.check(
                        database->update(*automatic, *employee, Employee{"Dora", 77}).has_value(),
                        "update is staged before destructor rollback");
                }
            }

            if (employee) {
                auto materialized = database->materialize(*employee);
                suite.check(materialized.has_value() && *materialized == Employee{"Dora", 5},
                            "rollback paths preserve the pre-existing object");
            }
            detach(database_id);
        }

        auto opened = Database::open(temporary.path());
        auto database = share(opened);
        suite.check(database != nullptr, "rollback database reopens");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(employee_builder()).has_value(), "rollback type is rebound");
        auto handle = database->get<Employee>(employee_id);
        suite.check(handle.has_value(), "pre-existing object survives reopening");
        if (handle) {
            auto employee = database->materialize(*handle);
            suite.check(employee.has_value() && *employee == Employee{"Dora", 5},
                        "rolled-back values remain absent after reopening");
        }
        detach(database_id);
    }

    // Contrato transact(): término normal com Ok faz commit; retorno de erro
    // reverte automaticamente (o mesmo contrato reusado pela Fase 9).
    {
        TemporaryDatabase temporary{"transact"};
        ObjectId committed_id{};
        ObjectId aborted_id{};
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            if (!database) {
                suite.check(false, "transact database is created");
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(employee_builder()).has_value(), "transact type is bound");

            // Caminho feliz: a função retorna Ok e transact faz o commit.
            auto ok = database->transact([&](Transaction& tx) -> Result<ObjectId> {
                auto handle = database->create(tx, Employee{"Faye", 1});
                if (!handle) {
                    return std::unexpected(handle.error());
                }
                return handle->id();
            });
            suite.check(ok.has_value(), "transact commits on normal completion");
            if (ok) {
                committed_id = *ok;
            }

            // Caminho de erro: a função retorna erro e transact faz o rollback.
            auto aborted = database->transact([&](Transaction& tx) -> Result<void> {
                auto handle = database->create(tx, Employee{"Gil", 2});
                if (!handle) {
                    return std::unexpected(handle.error());
                }
                aborted_id = handle->id();
                return std::unexpected(Error{ErrorCode::invalid_argument, "aborted on purpose"});
            });
            suite.check(!aborted && aborted.error().code == ErrorCode::invalid_argument,
                        "transact propagates the failure verbatim");
            suite.check_error(database->get<Employee>(aborted_id), ErrorCode::record_not_found,
                              "transact rolls back when the function returns an error");
            detach(database_id);
        }

        auto opened = Database::open(temporary.path());
        auto database = share(opened);
        suite.check(database != nullptr, "transact database reopens");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(employee_builder()).has_value(), "transact type is rebound");
        auto handle = database->get<Employee>(committed_id);
        suite.check(handle.has_value(), "the committed transact object survives reopening");
        if (handle) {
            auto employee = database->materialize(*handle);
            suite.check(employee.has_value() && *employee == Employee{"Faye", 1},
                        "transact commit persisted the right object");
        }
        suite.check_error(database->get<Employee>(aborted_id), ErrorCode::record_not_found,
                          "the aborted transact object is absent after reopening");
        detach(database_id);
    }

    return suite.finish();
}
