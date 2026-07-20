#include "modb/object/database.hpp"
#include "modb/storage/database_check.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
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

    // Um WAL estruturalmente ilegível não pode ser descartado automaticamente:
    // pode conter um commit durável que precisa de diagnóstico/repair.
    {
        TemporaryDatabase temporary{"corrupt-wal"};
        {
            auto created = Database::create(temporary.path());
            suite.check(created.has_value(), "database for corrupt WAL is created");
            if (!created) {
                return suite.finish();
            }
        }
        {
            std::ofstream wal{temporary.wal_path(), std::ios::binary | std::ios::trunc};
            wal.put('\x01');
            suite.check(wal.good(), "corrupt WAL header is written");
        }
        auto opened = Database::open(temporary.path());
        suite.check_error(opened, ErrorCode::wal_corrupt,
                          "opening with an unreadable WAL fails loudly");
        suite.check(std::filesystem::exists(temporary.wal_path()),
                    "an unreadable WAL is preserved for diagnosis");
    }

    // Um WAL de zero bytes pode resultar de criação interrompida antes do
    // header; não contém record algum e deve ser descartado como log vazio.
    {
        TemporaryDatabase temporary{"empty-wal"};
        {
            auto created = Database::create(temporary.path());
            suite.check(created.has_value(), "database for empty WAL is created");
            if (!created) {
                return suite.finish();
            }
        }
        {
            std::ofstream wal{temporary.wal_path(), std::ios::binary | std::ios::trunc};
            suite.check(wal.good(), "zero-byte WAL is created");
        }
        auto opened = Database::open(temporary.path());
        suite.check(opened.has_value(), "opening with a zero-byte WAL succeeds");
        // WAL v2 durável: arquivo vazio pode permanecer; não há registros a aplicar.
        suite.check(opened.has_value(), "zero-byte WAL does not block open");
    }

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
        suite.check(std::filesystem::exists(temporary.wal_path()),
                    "durable WAL remains after successful recovery");
        detach(database_id);
    }

    // Update também precisa sobreviver quando só o WAL possui o commit durável.
    {
        TemporaryDatabase temporary{"update-redo"};
        ObjectId employee_id{};
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            if (!database) {
                suite.check(false, "update-redo database is created");
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(employee_builder()).has_value(), "update-redo type is bound");
            auto seed = database->begin();
            if (!seed) {
                return suite.finish();
            }
            auto employee = database->create(*seed, Employee{"Before", 1});
            suite.check(employee.has_value() && seed->commit().has_value(),
                        "update-redo seed is committed");
            if (!employee) {
                return suite.finish();
            }
            employee_id = employee->id();
            auto update = database->begin();
            if (!update) {
                return suite.finish();
            }
            suite.check(database->update(*update, *employee, Employee{"After", 2}).has_value(),
                        "update-redo change is staged");
            suite.check(update->commit(CommitPhase::stop_after_commit_record).has_value(),
                        "update-redo commit record is durable before apply");
            detach(database_id);
        }
        auto opened = Database::open(temporary.path());
        auto database = share(opened);
        suite.check(database != nullptr, "update-redo database recovers");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(employee_builder()).has_value(), "update-redo type is rebound");
        auto handle = database->get<Employee>(employee_id);
        auto employee = handle ? database->materialize(*handle) : Result<Employee>{std::unexpected(handle.error())};
        suite.check(employee.has_value() && *employee == Employee{"After", 2},
                    "recovery reapplies the committed update");
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
        // WAL v2 durável: registros sem commit podem permanecer; não são aplicados.
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
        auto after_reopen = database->begin();
        suite.check(after_reopen.has_value(), "post-rollback transaction begins after reopening");
        if (after_reopen) {
            auto employee = database->create(*after_reopen, Employee{"Fresh", 6});
            suite.check(employee.has_value() && employee->id().value > rolled_back_id.value,
                        "durable rollback watermark prevents ObjectId reuse after reopening");
            suite.check(after_reopen->commit().has_value(), "post-rollback object commits");
        }
        detach(database_id);
    }

    // Contrato transact(): término normal com Ok faz commit; retorno de erro
    // reverte automaticamente (o mesmo contrato reusado pela Fase 9).
    {
        TemporaryDatabase temporary{"transact"};
        ObjectId committed_id{};
        ObjectId aborted_id{};
        ObjectId exception_id{};
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

            bool exception_propagated = false;
            try {
                (void)database->transact([&](Transaction& tx) -> Result<void> {
                    auto handle = database->create(tx, Employee{"Hana", 3});
                    if (handle) {
                        exception_id = handle->id();
                    }
                    throw std::runtime_error{"aborted by exception"};
                });
            } catch (const std::runtime_error&) {
                exception_propagated = true;
            }
            suite.check(exception_propagated, "transact propagates callback exceptions");
            suite.check_error(database->get<Employee>(exception_id), ErrorCode::record_not_found,
                              "transact rolls back when the callback throws");
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
        suite.check_error(database->get<Employee>(exception_id), ErrorCode::record_not_found,
                          "the exception-aborted transact object is absent after reopening");
        detach(database_id);
    }

    // Regressão: contadores em memória do TableHeap/IdentityMap não eram
    // revertidos no rollback (só o buffer de páginas do PageFile era
    // descartado), corrompendo a raiz do heap na escrita real seguinte; e o
    // contador de ObjectId revertia junto, permitindo que um id já entregue por
    // uma transação abortada fosse reatribuído. Corrigidos por
    // Database::resync_store_after_rollback + o watermark do ObjectId.
    {
        TemporaryDatabase temporary{"rollback-then-write"};
        std::vector<ObjectId> committed_ids;
        ObjectId rolled_back_id{};
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            if (!database) {
                suite.check(false, "regression database is created");
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(employee_builder()).has_value(),
                        "regression type is bound");

            // Uma transação que reverte, seguida por uma que de fato escreve —
            // o cenário que expôs o bug, repetido para várias páginas/rounds.
            for (int round = 0; round < 3; ++round) {
                auto doomed = database->begin();
                if (!doomed) {
                    suite.check(false, "doomed transaction begins");
                    detach(database_id);
                    return suite.finish();
                }
                auto transient =
                    database->create(*doomed, Employee{"Transient", static_cast<std::int64_t>(round)});
                if (transient) {
                    rolled_back_id = transient->id();
                }
                suite.check(doomed->rollback().has_value(), "doomed transaction rolls back");

                auto real = database->begin();
                if (!real) {
                    suite.check(false, "real transaction begins");
                    detach(database_id);
                    return suite.finish();
                }
                auto employee =
                    database->create(*real, Employee{"Real", static_cast<std::int64_t>(round)});
                if (!employee) {
                    suite.check(false, "real object is staged after a rollback");
                    detach(database_id);
                    return suite.finish();
                }
                committed_ids.push_back(employee->id());
                suite.check(real->commit().has_value(),
                            "real transaction commits after a preceding rollback");
            }

            bool any_reused = false;
            for (const auto id : committed_ids) {
                any_reused = any_reused || id == rolled_back_id;
            }
            suite.check(!any_reused,
                        "an ObjectId from a rolled-back transaction is never reused");

            bool all_correct = true;
            for (std::size_t index = 0; index < committed_ids.size(); ++index) {
                auto handle = database->get<Employee>(committed_ids[index]);
                auto employee = handle ? database->materialize(*handle) : Result<Employee>{
                    std::unexpected(Error{ErrorCode::record_not_found, "missing"})};
                all_correct = all_correct && employee.has_value() &&
                              employee->level == static_cast<std::int64_t>(index);
            }
            suite.check(all_correct,
                        "each real object after a rollback keeps its own value in the same session");
            detach(database_id);
        }

        // A raiz do heap não pode ter ficado corrompida pelas escritas
        // fantasmas das transações revertidas.
        auto checked = modb::storage::check_database(temporary.path());
        suite.check(checked.has_value() && checked->ok(),
                    "the data heap survives repeated rollback-then-write cycles uncorrupted");

        auto reopened = Database::open(temporary.path());
        auto database = share(reopened);
        suite.check(database != nullptr, "regression database reopens");
        if (database) {
            auto database_id = attach(database);
            suite.check(database->bind(employee_builder()).has_value(),
                        "regression type is rebound");
            bool all_correct_after_reopen = true;
            for (std::size_t index = 0; index < committed_ids.size(); ++index) {
                auto handle = database->get<Employee>(committed_ids[index]);
                auto employee = handle ? database->materialize(*handle) : Result<Employee>{
                    std::unexpected(Error{ErrorCode::record_not_found, "missing"})};
                all_correct_after_reopen = all_correct_after_reopen && employee.has_value() &&
                                           employee->level == static_cast<std::int64_t>(index);
            }
            suite.check(all_correct_after_reopen,
                        "every real object survives reopening with the right value");
            detach(database_id);
        }
    }

    return suite.finish();
}
