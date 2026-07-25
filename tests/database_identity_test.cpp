#include "modb/object/database.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <system_error>

using namespace modb;
using namespace modb::object;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-14-id-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + ".wal", ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

int main() {
    TestSuite suite;
    TemporaryDatabase temp{"uuid"};

    DatabaseUuid first{};
    {
        auto created = Database::create(temp.path());
        suite.check(created.has_value(), "create");
        if (!created) {
            return suite.finish();
        }
        first = created->database_uuid();
        suite.check(!first.is_nil(), "uuid assigned on create");
        suite.check(created->timeline_id().value == 1, "timeline starts at 1");
        suite.check(created->next_lsn() == 1, "next_lsn starts at 1");
    }

    {
        auto opened = Database::open(temp.path());
        suite.check(opened.has_value(), "reopen");
        if (opened) {
            suite.check(opened->database_uuid() == first, "uuid survives reopen");
            suite.check(opened->timeline_id().value == 1, "timeline survives reopen");
            suite.check(opened->next_lsn() == 1, "next_lsn survives reopen");
        }
    }

    // --- guardas de ciclo de vida: attach, begin/commit/rollback, GC ---
    {
        TemporaryDatabase guard_temp{"guards"};
        auto created = Database::create(guard_temp.path());
        suite.check(created.has_value(), "guards: database created");
        if (!created) {
            return suite.finish();
        }

        // Antes de anexado a um DatabaseRegistry, database_id_ fica em zero:
        // begin() e collect_garbage() rejeitam com a MESMA checagem que outros
        // métodos (create_index, snapshot) reusam, mas cada um em seu próprio
        // ponto de chamada — nenhum teste anterior chamava estes dois métodos
        // antes do attach.
        suite.check_error(created->begin(), ErrorCode::invalid_argument,
                          "guards: begin() before attach is rejected");
        suite.check_error(created->collect_garbage(), ErrorCode::invalid_argument,
                          "guards: collect_garbage() before attach is rejected");

        // DatabaseRegistry rejeita um ponteiro nulo antes de tentar atribuir um id.
        suite.check_error(DatabaseRegistry::instance().attach(std::shared_ptr<Database>{}),
                          ErrorCode::invalid_argument,
                          "guards: attaching a null database is rejected");

        auto db = std::make_shared<Database>(std::move(*created));
        auto database_id = DatabaseRegistry::instance().attach(db);
        suite.check(database_id.has_value(), "guards: database attached");
        if (!database_id) {
            return suite.finish();
        }

        auto tx = db->begin();
        suite.check(tx.has_value(), "guards: transaction begins after attach");
        if (tx) {
            // Uma segunda transação não pode começar por cima da primeira.
            suite.check_error(db->begin(), ErrorCode::transaction_active,
                              "guards: a nested begin() is rejected");
            // collect_garbage() também rejeita com uma transação em curso.
            suite.check_error(db->collect_garbage(), ErrorCode::transaction_active,
                              "guards: collect_garbage() during a transaction is rejected");

            suite.check(tx->commit().has_value(), "guards: transaction commits");
            // Commitar de novo a MESMA transação já concluída é rejeitado.
            suite.check_error(tx->commit(), ErrorCode::transaction_committed,
                              "guards: committing an already-committed transaction is rejected");
        }

        auto second_tx = db->begin();
        suite.check(second_tx.has_value(), "guards: a new transaction begins after the first ended");
        if (second_tx) {
            suite.check(second_tx->rollback().has_value(), "guards: transaction rolls back");
            // Commitar depois de já ter revertido é rejeitado (não mais ativa).
            suite.check_error(second_tx->commit(), ErrorCode::transaction_required,
                              "guards: committing after rollback is rejected");
        }
    }

    return suite.finish();
}
