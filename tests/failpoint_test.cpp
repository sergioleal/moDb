// Matriz de failpoints da Fase 5: cada cenário leva o commit a um ponto crítico
// e verifica, após reabrir com o arquivo real, que a transação aparece por
// completo ou não aparece — nunca pela metade.
//
// Dois mecanismos de "morte" são usados, ambos genuínos:
//  - Falha de I/O real: um FailpointWalSink faz uma escrita do WAL retornar
//    io_error (o processo "morre" no meio da gravação do log).
//  - Queda pós-sync (perda de energia): interrompe-se o commit num ponto (via
//    CommitPhase ou apply-failpoint) e "abandona-se" a instância — detach do
//    registro, para o destrutor da transação não reverter, e destrói-se o
//    Database sem aplicar/limpar. O buffer em memória some; o que estava no
//    disco (WAL/páginas) permanece, como numa queda real.
#include "modb/object/database.hpp"
#include "modb/storage/page_file.hpp"
#include "modb/tx/wal.hpp"
#include "failpoint_file.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
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
                ("modb-failpoint-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(wal_path(), ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path wal_path() const {
        auto result = path_;
        result += ".wal";
        return result;
    }

private:
    std::filesystem::path path_;
};

struct Item {
    std::int64_t sequence{};
    std::string payload;
    friend bool operator==(const Item&, const Item&) = default;
};

BindingBuilder<Item> item_builder() {
    BindingBuilder<Item> builder{"Item"};
    builder.field<1>("sequence", &Item::sequence).field<2>("payload", &Item::payload);
    return builder;
}

std::shared_ptr<Database> share(Result<Database>& result) {
    if (!result) {
        return {};
    }
    return std::make_shared<Database>(std::move(*result));
}

void detach(const Result<DatabaseId>& id) {
    if (id) {
        DatabaseRegistry::instance().detach(*id);
    }
}

Result<DatabaseId> attach(const std::shared_ptr<Database>& database) {
    return DatabaseRegistry::instance().attach(database);
}

void check_item(TestSuite& suite, const std::shared_ptr<Database>& database, ObjectId id,
                const Item& expected, std::string_view message) {
    auto handle = database->get<Item>(id);
    if (!handle) {
        suite.check(false, message);
        return;
    }
    auto item = database->materialize(*handle);
    suite.check(item.has_value() && *item == expected, message);
}

} // namespace

int main() {
    TestSuite suite;

    auto check_precommit_failure = [&](std::string_view suffix, tx::WalFileFactory factory,
                                       bool dirty_pages, std::string_view label) {
        TemporaryDatabase temporary{suffix};
        auto created = Database::create(temporary.path());
        auto database = share(created);
        suite.check(database != nullptr, std::string{label} + ": database is created");
        if (!database) {
            return;
        }
        auto database_id = attach(database);
        if (dirty_pages) {
            suite.check(database->bind(item_builder()).has_value(),
                        std::string{label} + ": type is bound");
        }
        database->set_wal_file_factory(std::move(factory));
        auto transaction = database->begin();
        suite.check(transaction.has_value(), std::string{label} + ": transaction begins");
        if (!transaction) {
            detach(database_id);
            return;
        }
        if (dirty_pages) {
            suite.check(database->create(*transaction, Item{9, "failpoint"}).has_value(),
                        std::string{label} + ": page image is staged");
        }
        auto committed = transaction->commit();
        suite.check(!committed && committed.error().code == ErrorCode::io_error,
                    std::string{label} + ": failure is not reported as durable");
        suite.check(transaction->rollback().has_value(),
                    std::string{label} + ": rollback remains available");
        suite.check(!std::filesystem::exists(temporary.wal_path()),
                    std::string{label} + ": rollback removes incomplete WAL");
        detach(database_id);
    };

    // (A) Falha de I/O REAL no WAL: uma escrita do log retorna io_error. O commit
    // falha, a transação é revertida no destrutor e o PageFile não fica preso em
    // transação (uma nova begin funciona). Após reabrir, o objeto nunca aparece.
    {
        TemporaryDatabase temporary{"wal-io-failure"};
        ObjectId failed_id{};
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            suite.check(database != nullptr, "io-failure database is created");
            if (!database) {
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(item_builder()).has_value(), "io-failure type is bound");

            // Injeta um WAL que falha logo após o cabeçalho (na escrita do begin).
            database->set_wal_file_factory(modb::test::failing_wal_factory(1));
            {
                auto transaction = database->begin();
                suite.check(transaction.has_value(), "io-failure transaction begins");
                if (!transaction) {
                    detach(database_id);
                    return suite.finish();
                }
                auto item = database->create(*transaction, Item{1, "doomed"});
                if (item) {
                    failed_id = item->id();
                }
                auto committed = transaction->commit();
                suite.check(!committed && committed.error().code == ErrorCode::io_error,
                            "commit reports the injected WAL I/O failure");
                // Deixa o destrutor reverter (transação ainda ativa após a falha).
            }
            suite.check(!std::filesystem::exists(temporary.wal_path()),
                        "the destructor rollback leaves no residual WAL");
            // O PageFile não ficou preso: uma nova transação abre normalmente.
            auto next = database->begin();
            suite.check(next.has_value(), "a new transaction begins after the failed commit");
            if (next) {
                suite.check(next->rollback().has_value(), "the probe transaction rolls back");
            }
            detach(database_id);
        }

        auto opened = Database::open(temporary.path());
        auto database = share(opened);
        suite.check(database != nullptr, "io-failure database reopens");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(item_builder()).has_value(), "io-failure type is rebound");
        suite.check_error(database->get<Item>(failed_id), ErrorCode::record_not_found,
                          "the rolled-back object never appears");
        detach(database_id);
    }

    // (A1) A primeira imagem de página é a terceira escrita (header, begin,
    // image). Sua falha ainda é pré-commit e reverte normalmente.
    check_precommit_failure("page-image-write-failure", modb::test::failing_wal_factory(2), true,
                            "page-image write failure");

    // (A2) A falha ao escrever o próprio registro de commit ocorre antes da
    // durabilidade: a transação continua ativa e pode fazer rollback normal.
    {
        TemporaryDatabase temporary{"commit-record-write-failure"};
        auto created = Database::create(temporary.path());
        auto database = share(created);
        suite.check(database != nullptr, "commit-record failure database is created");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        // Sem páginas sujas, as escritas são: header, begin, commit. Falhar
        // depois de duas permite provar exatamente a costura do commit record.
        database->set_wal_file_factory(modb::test::failing_wal_factory(2));
        auto transaction = database->begin();
        suite.check(transaction.has_value(), "commit-record failure transaction begins");
        if (!transaction) {
            detach(database_id);
            return suite.finish();
        }
        auto committed = transaction->commit();
        suite.check(!committed && committed.error().code == ErrorCode::io_error,
                    "failed commit-record write is not reported as durable");
        suite.check(transaction->rollback().has_value(),
                    "transaction rolls back after the failed commit-record write");
        suite.check(!std::filesystem::exists(temporary.wal_path()),
                    "rollback removes the WAL without a durable commit record");
        auto next = database->begin();
        suite.check(next.has_value(), "database remains usable after commit-record write failure");
        if (next) {
            (void)next->rollback();
        }
        detach(database_id);
    }

    // (A3/A4) Os dois syncs são costuras distintas: falhar antes ou depois de
    // gravar o record commit não pode marcar o commit como durável.
    check_precommit_failure("first-sync-failure", modb::test::failing_wal_factory(~std::size_t{0}, 0),
                            false, "first WAL sync failure");
    check_precommit_failure("commit-sync-failure", modb::test::failing_wal_factory(~std::size_t{0}, 1),
                            false, "commit WAL sync failure");

    // (B) Queda ANTES do registro de commit: imagens no WAL, sem commit. A
    // transação inteira deve permanecer ausente.
    {
        TemporaryDatabase temporary{"before-commit"};
        ObjectId item_id{};
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            if (!database) {
                suite.check(false, "before-commit database is created");
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(item_builder()).has_value(), "before-commit type is bound");
            auto transaction = database->begin();
            if (!transaction) {
                suite.check(false, "before-commit transaction begins");
                detach(database_id);
                return suite.finish();
            }
            auto item = database->create(*transaction, Item{1, "not committed"});
            if (item) {
                item_id = item->id();
            }
            suite.check(item.has_value(), "before-commit item is staged");
            suite.check(transaction->commit(CommitPhase::stop_after_images).has_value(),
                        "process stops before appending commit");
            // Queda: abandona sem reverter (o WAL sem commit fica no disco).
            detach(database_id);
        }

        auto opened = Database::open(temporary.path());
        auto database = share(opened);
        suite.check(database != nullptr, "before-commit database recovers");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(item_builder()).has_value(), "before-commit type is rebound");
        suite.check_error(database->get<Item>(item_id), ErrorCode::record_not_found,
                          "transaction is wholly absent before commit");
        detach(database_id);
    }

    // (C) Queda logo APÓS o commit durável, antes de aplicar: a abertura refaz a
    // transação completa (redo).
    {
        TemporaryDatabase temporary{"after-commit"};
        ObjectId item_id{};
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            if (!database) {
                suite.check(false, "after-commit database is created");
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(item_builder()).has_value(), "after-commit type is bound");
            auto transaction = database->begin();
            if (!transaction) {
                suite.check(false, "after-commit transaction begins");
                detach(database_id);
                return suite.finish();
            }
            auto item = database->create(*transaction, Item{2, "durable WAL"});
            if (item) {
                item_id = item->id();
            }
            suite.check(item.has_value(), "after-commit item is staged");
            suite.check(transaction->commit(CommitPhase::stop_after_commit_record).has_value(),
                        "process stops after durable commit");
            detach(database_id);
        }

        auto opened = Database::open(temporary.path());
        auto database = share(opened);
        suite.check(database != nullptr, "after-commit database recovers");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(item_builder()).has_value(), "after-commit type is rebound");
        check_item(suite, database, item_id, Item{2, "durable WAL"},
                   "committed transaction is wholly present");
        detach(database_id);
    }

    // (D) Falha no MEIO da aplicação depois do commit durável: o apply-failpoint
    // aplica só uma página e retorna io_error. O destrutor real da Transaction
    // roda; ele não pode apagar o WAL já commitado. A instância fica envenenada
    // até reabrir e recovery reaplica tudo.
    {
        TemporaryDatabase temporary{"during-apply"};
        std::vector<ObjectId> ids;
        constexpr std::size_t item_count = 80;
        const std::string payload(180, 'x');
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            if (!database) {
                suite.check(false, "during-apply database is created");
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(item_builder()).has_value(), "during-apply type is bound");
            {
            auto transaction = database->begin();
            if (!transaction) {
                suite.check(false, "during-apply transaction begins");
                detach(database_id);
                return suite.finish();
            }
            ids.reserve(item_count);
            for (std::size_t index = 0; index < item_count; ++index) {
                auto item = database->create(
                    *transaction, Item{static_cast<std::int64_t>(index), payload});
                if (!item) {
                    suite.check(false, "all during-apply items are staged");
                    break;
                }
                ids.push_back(item->id());
            }
            suite.check(ids.size() == item_count, "transaction dirties multiple pages");
            // Aplica só 1 página e então falha, simulando a queda no meio do apply.
            database->set_apply_failpoint(1);
            auto committed = transaction->commit();
            suite.check(!committed && committed.error().code == ErrorCode::commit_recovery_required,
                        "apply failure after durable commit requires recovery");
            suite.check(std::filesystem::exists(temporary.wal_path()),
                        "the durable WAL remains before the transaction destructor runs");
            suite.check_error(transaction->rollback(), ErrorCode::transaction_committed,
                              "a durably committed transaction cannot roll back");
            // O objeto Transaction sai de escopo normalmente: não há detach nem
            // std::exit para esconder um rollback que apagaria o WAL.
            }
            suite.check(std::filesystem::exists(temporary.wal_path()),
                        "the transaction destructor preserves the durable WAL");
            auto blocked = database->begin();
            suite.check_error(blocked, ErrorCode::database_recovery_required,
                              "database rejects new work until it is reopened");
            detach(database_id);
        }

        auto opened = Database::open(temporary.path());
        auto database = share(opened);
        suite.check(database != nullptr, "partially applied database recovers");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(item_builder()).has_value(), "during-apply type is rebound");
        bool all_present = ids.size() == item_count;
        for (std::size_t index = 0; index < ids.size(); ++index) {
            auto handle = database->get<Item>(ids[index]);
            if (!handle) {
                all_present = false;
                break;
            }
            auto item = database->materialize(*handle);
            if (!item || item->sequence != static_cast<std::int64_t>(index) ||
                item->payload != payload) {
                all_present = false;
                break;
            }
        }
        suite.check(all_present, "recovery completes every page of a partially applied commit");
        detach(database_id);
    }

    // (E) Queda durante o checkpoint: páginas aplicadas, WAL residual. A abertura
    // reaplica (idempotente) e remove o WAL, sem duplicar efeitos.
    {
        TemporaryDatabase temporary{"during-checkpoint"};
        ObjectId item_id{};
        {
            auto created = Database::create(temporary.path());
            auto database = share(created);
            if (!database) {
                suite.check(false, "checkpoint database is created");
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(item_builder()).has_value(), "checkpoint type is bound");
            auto transaction = database->begin();
            if (!transaction) {
                suite.check(false, "checkpoint transaction begins");
                detach(database_id);
                return suite.finish();
            }
            auto item = database->create(*transaction, Item{3, "already applied"});
            if (item) {
                item_id = item->id();
            }
            suite.check(item.has_value(), "checkpoint item is staged");
            suite.check(transaction->commit(CommitPhase::stop_before_wal_cleanup).has_value(),
                        "process stops with applied pages and residual WAL");
            suite.check(std::filesystem::exists(temporary.wal_path()),
                        "checkpoint interruption leaves the WAL");
            detach(database_id);
        }

        auto opened = Database::open(temporary.path());
        auto database = share(opened);
        suite.check(database != nullptr, "checkpoint-interrupted database recovers");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(item_builder()).has_value(), "checkpoint type is rebound");
        check_item(suite, database, item_id, Item{3, "already applied"},
                   "checkpoint recovery preserves the committed item");
        suite.check(std::filesystem::exists(temporary.wal_path()),
                    "checkpoint recovery keeps the durable WAL");
        detach(database_id);
    }

    return suite.finish();
}
