// Matriz de integração e recuperação da Fase 6D: junta snapshots (6B),
// coleta de lixo (6C), transações/WAL (5) e recuperação para provar que o
// modelo MVCC completo não produz leitura mista, versão perdida, vazamento
// persistente ou corrupção após recovery (critério da Fase 6).
#include "modb/object/database.hpp"
#include "modb/storage/database_check.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
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
                ("modb-mvcc-rec-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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

struct Doc {
    std::string title;
    std::int64_t rev{};
    friend bool operator==(const Doc&, const Doc&) = default;
};

BindingBuilder<Doc> doc_builder() {
    BindingBuilder<Doc> builder{"Doc"};
    builder.field<1>("title", &Doc::title).field<2>("rev", &Doc::rev);
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

Result<ObjectId> create_committed(Database& database, Doc value) {
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

Result<void> set_rev(Database& database, ObjectId id, std::int64_t rev) {
    auto tx = database.begin();
    if (!tx) {
        return std::unexpected(tx.error());
    }
    auto handle = database.get<Doc>(id);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    if (auto set = handle->set<&Doc::rev>(*tx, rev); !set) {
        return std::unexpected(set.error());
    }
    return tx->commit();
}

Result<void> remove_committed(Database& database, ObjectId id) {
    auto tx = database.begin();
    if (!tx) {
        return std::unexpected(tx.error());
    }
    if (auto removed = database.remove(*tx, id); !removed) {
        return std::unexpected(removed.error());
    }
    return tx->commit();
}

} // namespace

int main() {
    TestSuite suite;

    // === A. Consulta longa: uma leitura estável atravessa muitas mutações ===
    // Um snapshot aberto antes de uma sequência de create/update/remove
    // intercalados continua vendo exatamente o estado lógico da sua época.
    {
        TemporaryDatabase temp{"long-read"};
        auto created = Database::create(temp.path());
        auto database = share(created);
        suite.check(database != nullptr, "A: long-read database is created");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database_id.has_value(), "A: database is attached");
        suite.check(database->bind(doc_builder()).has_value(), "A: Doc is bound");

        auto a = create_committed(*database, Doc{"alpha", 1});
        auto b = create_committed(*database, Doc{"bravo", 1});
        auto c = create_committed(*database, Doc{"charlie", 1});
        suite.check(a.has_value() && b.has_value() && c.has_value(),
                    "A: three documents are created and committed");
        if (a && b && c) {
            suite.check(database->data_record_count() == 3,
                        "A: exactly three physical records before the snapshot");

            auto opened = database->snapshot();
            suite.check(opened.has_value(), "A: a snapshot opens over the initial state");
            if (opened) {
                Snapshot view = std::move(*opened);

                // Sequência intercalada de mutações, cada uma num commit próprio.
                suite.check(set_rev(*database, *a, 2).has_value(), "A: alpha is updated");
                suite.check(remove_committed(*database, *b).has_value(), "A: bravo is removed");
                auto d = create_committed(*database, Doc{"delta", 1});
                suite.check(d.has_value(), "A: delta is created after the snapshot");
                suite.check(set_rev(*database, *c, 2).has_value(), "A: charlie is updated");

                // A leitura pelo snapshot ainda enxerga o estado da época.
                auto a_at = database->get<Doc>(*a, view);
                auto b_at = database->get<Doc>(*b, view);
                auto c_at = database->get<Doc>(*c, view);
                suite.check(a_at.has_value() && a_at->rev == 1,
                            "A: the snapshot still sees alpha's original revision");
                suite.check(b_at.has_value() && b_at->title == "bravo",
                            "A: the snapshot still sees bravo after its removal");
                suite.check(c_at.has_value() && c_at->rev == 1,
                            "A: the snapshot still sees charlie's original revision");
                if (d) {
                    suite.check_error(database->get<Doc>(*d, view), ErrorCode::record_not_found,
                                      "A: the snapshot never sees a document created later");
                }

                std::set<std::string> at_snapshot;
                auto scanned = database->scan<Doc>(view, [&](const Doc& doc) -> Result<void> {
                    at_snapshot.insert(doc.title);
                    return {};
                });
                suite.check(scanned.has_value(), "A: scan through the snapshot succeeds");
                suite.check(at_snapshot.size() == 3 && at_snapshot.contains("alpha") &&
                                at_snapshot.contains("bravo") && at_snapshot.contains("charlie") &&
                                !at_snapshot.contains("delta"),
                            "A: scan enumerates exactly the epoch's live set");

                // GC com o snapshot aberto não recupera nada (retenção).
                auto retained = database->collect_garbage();
                suite.check(retained.has_value() && *retained == 0,
                            "A: gc reclaims nothing while the long read is open");
            }

            // Snapshot fechado: o GC recupera as versões antigas retidas
            // (alpha v1, bravo, charlie v1) — nem versão perdida, nem vazamento.
            auto collected = database->collect_garbage();
            suite.check(collected.has_value() && *collected == 3,
                        "A: gc reclaims the three retained versions once the read closes");
            suite.check(database->data_record_count() == 3,
                        "A: only the current live versions remain physically after gc");

            // O estado corrente reflete todas as mutações: alpha/charlie na
            // revisão nova, delta presente, bravo removido.
            std::set<std::string> live_now;
            auto opened_now = database->snapshot();
            suite.check(opened_now.has_value(), "A: a fresh snapshot opens on the current state");
            if (opened_now) {
                Snapshot now = std::move(*opened_now);
                auto scan_now = database->scan<Doc>(now, [&](const Doc& doc) -> Result<void> {
                    live_now.insert(doc.title);
                    return {};
                });
                suite.check(scan_now.has_value(), "A: the current scan succeeds");
            }
            suite.check(live_now.size() == 3 && live_now.contains("alpha") &&
                            live_now.contains("charlie") && live_now.contains("delta") &&
                            !live_now.contains("bravo"),
                        "A: the current state reflects every committed mutation");
        }
        // Fecha a instância (desanexa do registro e solta o shared_ptr) antes de
        // o check reabrir o arquivo — evita conflito de compartilhamento no
        // Windows e garante que ele leia o estado consolidado em disco.
        detach(database_id);
        database.reset();
        auto checked = modb::storage::check_database(temp.path());
        suite.check(checked.has_value() && checked->ok(),
                    "A: the database is structurally sound after the long read and gc");
    }

    // === B. Durabilidade versionada + sem vazamento através da reabertura ===
    {
        TemporaryDatabase temp{"reopen"};
        ObjectId doc_id{};
        std::uint64_t epoch_before_close = 0;
        {
            auto created = Database::create(temp.path());
            auto database = share(created);
            suite.check(database != nullptr, "B: reopen database is created");
            if (!database) {
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(doc_builder()).has_value(), "B: Doc is bound");
            auto id = create_committed(*database, Doc{"report", 1});
            suite.check(id.has_value(), "B: the document is created and committed");
            if (!id) {
                detach(database_id);
                return suite.finish();
            }
            doc_id = *id;
            suite.check(set_rev(*database, doc_id, 2).has_value(),
                        "B: the document is updated (a previous version is retained)");
            suite.check(database->data_record_count() == 2,
                        "B: both versions are physically present before reopening");
            epoch_before_close = database->epoch();
            detach(database_id);
        }

        auto opened = Database::open(temp.path());
        auto database = share(opened);
        suite.check(database != nullptr, "B: database reopens cleanly");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(doc_builder()).has_value(), "B: Doc is rebound after reopening");
        suite.check(database->epoch() >= epoch_before_close,
                    "B: the epoch stays monotonic across reopening");
        // A versão corrente sobreviveu; nenhuma versão foi perdida.
        auto handle = database->get<Doc>(doc_id);
        Result<Doc> value{std::unexpected(Error{ErrorCode::record_not_found, "x"})};
        if (handle) {
            value = database->materialize(*handle);
        }
        suite.check(value.has_value() && value->rev == 2,
                    "B: the current version survives reopening intact");
        // A versão previous sobreviveu no disco; sem snapshot, é recuperável.
        suite.check(database->data_record_count() == 2,
                    "B: the retained previous version is still on disk after reopening");
        auto reclaimed = database->collect_garbage();
        suite.check(reclaimed.has_value() && *reclaimed == 1,
                    "B: gc after reopening reclaims the now-orphaned previous version");
        suite.check(database->data_record_count() == 1,
                    "B: no persistent leak — only the current version remains");
        auto after = database->get<Doc>(doc_id);
        Result<Doc> after_value{std::unexpected(Error{ErrorCode::record_not_found, "x"})};
        if (after) {
            after_value = database->materialize(*after);
        }
        suite.check(after_value.has_value() && after_value->rev == 2,
                    "B: gc after reopening does not disturb the current version");
        detach(database_id);
    }

    // === C. Recovery de um UPDATE versionado com commit durável, não aplicado ===
    {
        TemporaryDatabase temp{"update-redo"};
        ObjectId doc_id{};
        {
            auto created = Database::create(temp.path());
            auto database = share(created);
            suite.check(database != nullptr, "C: update-redo database is created");
            if (!database) {
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(doc_builder()).has_value(), "C: Doc is bound");
            auto id = create_committed(*database, Doc{"draft", 1});
            suite.check(id.has_value(), "C: the seed document is committed");
            if (!id) {
                detach(database_id);
                return suite.finish();
            }
            doc_id = *id;
            // Update com commit durável no WAL, mas queda antes de aplicar.
            auto tx = database->begin();
            suite.check(tx.has_value(), "C: the update transaction begins");
            if (tx) {
                auto handle = database->get<Doc>(doc_id);
                suite.check(handle.has_value() &&
                                handle->set<&Doc::rev>(*tx, 2).has_value(),
                            "C: the update is staged");
                suite.check(tx->commit(CommitPhase::stop_after_commit_record).has_value(),
                            "C: the update commit record is durable before applying");
            }
            detach(database_id);  // abandona a instância: a queda é simulada
        }

        auto opened = Database::open(temp.path());
        auto database = share(opened);
        suite.check(database != nullptr, "C: database recovers on reopening");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(doc_builder()).has_value(), "C: Doc is rebound after recovery");
        auto handle = database->get<Doc>(doc_id);
        Result<Doc> value{std::unexpected(Error{ErrorCode::record_not_found, "x"})};
        if (handle) {
            value = database->materialize(*handle);
        }
        suite.check(value.has_value() && value->rev == 2,
                    "C: recovery reapplies the versioned update — no lost version");
        detach(database_id);
        database.reset();
        auto checked = modb::storage::check_database(temp.path());
        suite.check(checked.has_value() && checked->ok(),
                    "C: the database is structurally sound after recovery");
    }

    // === D. Recovery de um REMOVE versionado com commit durável, não aplicado ===
    {
        TemporaryDatabase temp{"remove-redo"};
        ObjectId doc_id{};
        {
            auto created = Database::create(temp.path());
            auto database = share(created);
            suite.check(database != nullptr, "D: remove-redo database is created");
            if (!database) {
                return suite.finish();
            }
            auto database_id = attach(database);
            suite.check(database->bind(doc_builder()).has_value(), "D: Doc is bound");
            auto id = create_committed(*database, Doc{"obsolete", 1});
            suite.check(id.has_value(), "D: the seed document is committed");
            if (!id) {
                detach(database_id);
                return suite.finish();
            }
            doc_id = *id;
            auto tx = database->begin();
            suite.check(tx.has_value(), "D: the removal transaction begins");
            if (tx) {
                suite.check(database->remove(*tx, doc_id).has_value(), "D: the removal is staged");
                suite.check(tx->commit(CommitPhase::stop_after_commit_record).has_value(),
                            "D: the removal commit record is durable before applying");
            }
            detach(database_id);
        }

        auto opened = Database::open(temp.path());
        auto database = share(opened);
        suite.check(database != nullptr, "D: database recovers on reopening");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database->bind(doc_builder()).has_value(), "D: Doc is rebound after recovery");
        suite.check_error(database->get<Doc>(doc_id), ErrorCode::record_not_found,
                          "D: recovery reapplies the versioned removal");
        detach(database_id);
        database.reset();
        auto checked = modb::storage::check_database(temp.path());
        suite.check(checked.has_value() && checked->ok(),
                    "D: the database is structurally sound after recovering a removal");
    }

    return suite.finish();
}
