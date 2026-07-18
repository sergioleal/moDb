// Exercita o Snapshot da Fase 6B: leituras estáveis através de get/scan
// versionados, imunes a commits feitos depois que o snapshot foi aberto, e o
// conflito de uma segunda alteração enquanto uma versão previous ainda é
// visível a um snapshot mais antigo (ADR-009).
#include "modb/object/database.hpp"
#include "test_support.hpp"

#include <chrono>
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
                ("modb-snapshot-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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

struct Account {
    std::string owner;
    std::int64_t balance{};
    friend bool operator==(const Account&, const Account&) = default;
};

BindingBuilder<Account> account_builder() {
    BindingBuilder<Account> builder{"Account"};
    builder.field<1>("owner", &Account::owner).field<2>("balance", &Account::balance);
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

// Altera o saldo de um Account numa transação própria e commita.
Result<void> set_balance(Database& database, ObjectId id, std::int64_t balance) {
    auto tx = database.begin();
    if (!tx) {
        return std::unexpected(tx.error());
    }
    auto handle = database.get<Account>(id);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    if (auto set = handle->set<&Account::balance>(*tx, balance); !set) {
        return std::unexpected(set.error());
    }
    return tx->commit();
}

// Cria um Account numa transação própria e commita; devolve o id.
Result<ObjectId> create_committed(Database& database, Account value) {
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

} // namespace

int main() {
    TestSuite suite;
    TemporaryDatabase temp{"main"};

    auto created = Database::create(temp.path());
    auto database = share(created);
    suite.check(database != nullptr, "snapshot database is created");
    if (!database) {
        return suite.finish();
    }
    auto database_id = attach(database);
    suite.check(database_id.has_value(), "snapshot database is attached");
    suite.check(database->bind(account_builder()).has_value(), "Account is bound");

    // --- leitura estável: snapshot não vê um update posterior ---
    auto ana_id = create_committed(*database, Account{"Ana", 100});
    suite.check(ana_id.has_value(), "Ana is created and committed");
    if (!ana_id) {
        return suite.finish();
    }
    {
        // Escopo próprio: o snapshot desregistra sua época ao final do bloco,
        // para não continuar "aberto" e interferir nas seções seguintes.
        auto opened = database->snapshot();
        suite.check(opened.has_value(), "a snapshot opens over Ana's initial state");
        if (!opened) {
            return suite.finish();
        }
        Snapshot stable = std::move(*opened);
        {
            auto tx = database->begin();
            suite.check(tx.has_value(), "update transaction begins");
            if (tx) {
                auto handle = database->get<Account>(*ana_id);
                suite.check(handle.has_value(), "Ana's handle resolves");
                if (handle) {
                    suite.check(handle->set<&Account::balance>(*tx, 200).has_value(),
                                "Ana's balance is updated to 200");
                }
                suite.check(tx->commit().has_value(), "update transaction commits");
            }
        }
        auto ana_via_snapshot = database->get<Account>(*ana_id, stable);
        suite.check(ana_via_snapshot.has_value() && ana_via_snapshot->balance == 100,
                    "the snapshot still sees the balance from before the update");
        auto ana_current = database->get<Account>(*ana_id);
        Result<Account> ana_current_value{
            std::unexpected(Error{ErrorCode::record_not_found, "x"})};
        if (ana_current) {
            ana_current_value = database->materialize(*ana_current);
        }
        suite.check(ana_current_value.has_value() && ana_current_value->balance == 200,
                    "a plain read (no snapshot) sees the updated balance");
    }

    // --- remoção invisível: snapshot ainda vê o objeto removido depois ---
    auto bia_id = create_committed(*database, Account{"Bia", 50});
    suite.check(bia_id.has_value(), "Bia is created and committed");
    if (bia_id) {
        auto opened = database->snapshot();
        suite.check(opened.has_value(), "a snapshot opens before Bia's removal");
        if (opened) {
            Snapshot before_removal = std::move(*opened);
            auto tx = database->begin();
            suite.check(tx.has_value(), "removal transaction begins");
            if (tx) {
                suite.check(database->remove(*tx, *bia_id).has_value(), "Bia is removed");
                suite.check(tx->commit().has_value(), "removal transaction commits");
            }
            auto bia_via_snapshot = database->get<Account>(*bia_id, before_removal);
            suite.check(bia_via_snapshot.has_value() && bia_via_snapshot->owner == "Bia",
                        "the snapshot still sees Bia after her removal");
            suite.check_error(database->get<Account>(*bia_id), ErrorCode::record_not_found,
                              "a plain read (no snapshot) no longer finds the removed Bia");
        }
    }

    // --- criação invisível: snapshot aberto antes não vê um objeto criado depois ---
    Result<ObjectId> caio_id{std::unexpected(Error{ErrorCode::record_not_found, "x"})};
    {
        auto opened = database->snapshot();
        suite.check(opened.has_value(), "a snapshot opens before Caio is created");
        caio_id = create_committed(*database, Account{"Caio", 300});
        suite.check(caio_id.has_value(), "Caio is created and committed");
        if (opened && caio_id) {
            Snapshot before_creation = std::move(*opened);
            suite.check_error(database->get<Account>(*caio_id, before_creation),
                              ErrorCode::record_not_found,
                              "a snapshot opened before creation never sees the new object");
            auto caio_current = database->get<Account>(*caio_id);
            suite.check(caio_current.has_value(), "a plain read sees Caio right away");
        }
    }

    // --- scan consistente: enumera exatamente o estado da época capturada ---
    // Neste ponto Bia já foi removida (seção anterior), então o conjunto vivo
    // na época do snapshot é {Ana, Caio}, sem Bia e sem Dora.
    {
        auto opened = database->snapshot();
        suite.check(opened.has_value(), "a snapshot opens for the scan check");
        auto dora_id = create_committed(*database, Account{"Dora", 400});
        suite.check(dora_id.has_value(), "Dora is created after the scan snapshot");
        if (opened) {
            Snapshot scan_snapshot = std::move(*opened);
            std::set<std::string> owners_at_snapshot;
            auto scanned = database->scan<Account>(
                scan_snapshot, [&](const Account& account) -> Result<void> {
                    owners_at_snapshot.insert(account.owner);
                    return {};
                });
            suite.check(scanned.has_value(), "scan through the snapshot succeeds");
            suite.check(owners_at_snapshot.contains("Ana") &&
                            !owners_at_snapshot.contains("Bia") &&
                            owners_at_snapshot.contains("Caio") &&
                            !owners_at_snapshot.contains("Dora"),
                        "scan enumerates exactly the accounts alive at the snapshot's epoch");
        }
    }

    // --- conflito: segunda alteração enquanto a previous ainda é visível ---
    auto eva_id = create_committed(*database, Account{"Eva", 1});
    suite.check(eva_id.has_value(), "Eva is created and committed");
    if (eva_id) {
        {
            // O snapshot vive só neste escopo: sai (e desregistra sua época)
            // exatamente ao final deste bloco.
            auto opened = database->snapshot();
            suite.check(opened.has_value(), "a snapshot opens over Eva's original balance");
            if (!opened) {
                detach(database_id);
                return suite.finish();
            }
            Snapshot old_snapshot = std::move(*opened);

            // Primeira alteração: segura, cria a única previous disponível.
            {
                auto tx = database->begin();
                suite.check(tx.has_value(), "first-update transaction begins");
                if (tx) {
                    auto handle = database->get<Account>(*eva_id);
                    suite.check(handle.has_value() &&
                                    handle->set<&Account::balance>(*tx, 2).has_value(),
                                "the first update after opening the snapshot succeeds");
                    suite.check(tx->commit().has_value(), "first update commits");
                }
            }

            // Segunda alteração com o snapshot antigo AINDA aberto: sem espaço
            // para uma terceira versão -> snapshot_conflict, sem escrita parcial.
            {
                auto tx = database->begin();
                suite.check(tx.has_value(), "conflicting transaction begins");
                if (tx) {
                    auto handle = database->get<Account>(*eva_id);
                    suite.check(handle.has_value(),
                                "Eva's handle resolves for the conflict attempt");
                    if (handle) {
                        suite.check_error(
                            handle->set<&Account::balance>(*tx, 3), ErrorCode::snapshot_conflict,
                            "a second change while the old snapshot is open conflicts");
                    }
                    suite.check(tx->rollback().has_value(),
                                "the conflicting transaction rolls back");
                }
            }
            auto eva_after_conflict = database->get<Account>(*eva_id);
            Result<Account> eva_value{std::unexpected(Error{ErrorCode::record_not_found, "x"})};
            if (eva_after_conflict) {
                eva_value = database->materialize(*eva_after_conflict);
            }
            suite.check(eva_value.has_value() && eva_value->balance == 2,
                        "the rejected write leaves no partial change");
            (void)old_snapshot;
        }

        // O snapshot antigo já fechou: agora a mesma alteração é permitida.
        auto tx = database->begin();
        suite.check(tx.has_value(), "unblocked transaction begins");
        if (tx) {
            auto handle = database->get<Account>(*eva_id);
            suite.check(handle.has_value() &&
                            handle->set<&Account::balance>(*tx, 3).has_value(),
                        "the same change succeeds once the old snapshot is closed");
            suite.check(tx->commit().has_value(), "the now-unblocked update commits");
        }
    }

    // --- Fase 6C: retenção e coleta de lixo das versões previous ---
    // Banco próprio, para as contagens físicas serem determinísticas e não
    // dependerem do que as seções anteriores deixaram.
    {
        TemporaryDatabase gc_temp{"gc"};
        auto gc_created = Database::create(gc_temp.path());
        auto gc_db = share(gc_created);
        suite.check(gc_db != nullptr, "gc database is created");
        if (gc_db) {
            auto gc_id = attach(gc_db);
            suite.check(gc_id.has_value(), "gc database is attached");
            suite.check(gc_db->bind(account_builder()).has_value(),
                        "Account is bound in the gc database");

            auto fabio = create_committed(*gc_db, Account{"Fabio", 10});
            suite.check(fabio.has_value(), "Fabio is created and committed");
            if (fabio) {
                // Só o registro corrente de Fabio existe fisicamente.
                const auto baseline = gc_db->data_record_count();
                suite.check(baseline == 1, "one physical record after creation");

                {
                    // Snapshot aberto ANTES do update: a versão previous que ele
                    // criar precisa sobreviver enquanto este snapshot viver.
                    auto opened = gc_db->snapshot();
                    suite.check(opened.has_value(), "a snapshot opens before Fabio's update");
                    if (!opened) {
                        detach(gc_id);
                        detach(database_id);
                        return suite.finish();
                    }
                    Snapshot keep = std::move(*opened);

                    suite.check(set_balance(*gc_db, *fabio, 20).has_value(),
                                "Fabio's balance is updated to 20");
                    suite.check(gc_db->data_record_count() == baseline + 1,
                                "the update leaves the previous version physically present");

                    // GC com o snapshot aberto: a versão previous ainda é
                    // visível a ele, então nada é recuperado (retenção).
                    auto retained = gc_db->collect_garbage();
                    suite.check(retained.has_value() && *retained == 0,
                                "gc reclaims nothing while a snapshot can still see the previous");
                    suite.check(gc_db->data_record_count() == baseline + 1,
                                "the previous version survives gc while the snapshot is open");
                    auto via_snapshot = gc_db->get<Account>(*fabio, keep);
                    suite.check(via_snapshot.has_value() && via_snapshot->balance == 10,
                                "the snapshot still reads Fabio's original balance");
                }

                // Snapshot fechado: a versão previous passa a ser coletável.
                auto collected = gc_db->collect_garbage();
                suite.check(collected.has_value() && *collected == 1,
                            "gc reclaims the previous version once the snapshot is closed");
                suite.check(gc_db->data_record_count() == baseline,
                            "only Fabio's current version remains physically");
                auto current = gc_db->get<Account>(*fabio);
                Result<Account> current_value{
                    std::unexpected(Error{ErrorCode::record_not_found, "x"})};
                if (current) {
                    current_value = gc_db->materialize(*current);
                }
                suite.check(current_value.has_value() && current_value->balance == 20,
                            "gc does not disturb the current version");

                // Duas alterações SEM snapshot: a segunda sobrescreve a posição
                // previous, deixando a versão intermediária órfã no heap. O GC
                // recupera tanto a previous referenciada quanto a órfã.
                suite.check(set_balance(*gc_db, *fabio, 30).has_value(), "Fabio updated to 30");
                suite.check(set_balance(*gc_db, *fabio, 40).has_value(), "Fabio updated to 40");
                suite.check(gc_db->data_record_count() == baseline + 2,
                            "two snapshotless updates leave a previous and an orphan copy");
                auto swept = gc_db->collect_garbage();
                suite.check(swept.has_value() && *swept == 2,
                            "gc reclaims both the previous and the orphaned intermediate copy");
                suite.check(gc_db->data_record_count() == baseline,
                            "only the latest version remains after the sweep");
                auto latest = gc_db->get<Account>(*fabio);
                Result<Account> latest_value{
                    std::unexpected(Error{ErrorCode::record_not_found, "x"})};
                if (latest) {
                    latest_value = gc_db->materialize(*latest);
                }
                suite.check(latest_value.has_value() && latest_value->balance == 40,
                            "the current version is the latest committed value after gc");
            }
            detach(gc_id);
        }
    }

    detach(database_id);
    return suite.finish();
}
