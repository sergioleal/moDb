#include "modb/object/database.hpp"
#include "modb/object/binding.hpp"
#include "modb/tx/wal.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>

using namespace modb;
using namespace modb::object;

namespace {

struct Emp {
    std::string name;
};

BindingBuilder<Emp> emp_builder() {
    BindingBuilder<Emp> builder{"Employee"};
    builder.field<1>("name", &Emp::name);
    return builder;
}

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-14b-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + ".wal", ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path wal_path() const { return path_.string() + ".wal"; }

private:
    std::filesystem::path path_;
};

std::shared_ptr<Database> share(Result<Database>& result) {
    if (!result) {
        return {};
    }
    return std::make_shared<Database>(std::move(*result));
}

Result<DatabaseId> attach(const std::shared_ptr<Database>& database) {
    return DatabaseRegistry::instance().attach(database);
}

void detach(DatabaseId id) { (void)DatabaseRegistry::instance().detach(id); }

} // namespace

int main() {
    TestSuite suite;
    TemporaryDatabase temp{"durable"};

    std::uint64_t lsn_after_first = 0;
    {
        auto created = Database::create(temp.path());
        auto database = share(created);
        suite.check(database != nullptr, "create");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database_id.has_value(), "attach1");
        if (!database_id) {
            return suite.finish();
        }
        suite.check(database->bind(emp_builder()).has_value(), "bind");
        auto tx = database->begin();
        suite.check(tx.has_value(), "begin1");
        if (tx) {
            suite.check(database->create(*tx, Emp{"alice"}).has_value(), "create alice");
            suite.check(tx->commit().has_value(), "commit1");
        }
        suite.check(std::filesystem::exists(temp.wal_path()), "wal exists after commit");
        lsn_after_first = database->next_lsn();
        suite.check(lsn_after_first > 1, "next_lsn advanced");
        suite.check(database->checkpoint_lsn() > 0, "checkpoint advanced");
        suite.check(database->oldest_available_lsn() == database->checkpoint_lsn(),
                    "oldest==checkpoint");

        auto records = tx::Wal::read_all(temp.wal_path());
        suite.check(records.has_value(), "read_all");
        if (records) {
            bool saw_commit = false;
            for (const auto& r : *records) {
                if (r.type == tx::WalRecordType::commit) {
                    saw_commit = true;
                    suite.check(r.commit_lsn() == r.lsn, "commit_lsn matches record lsn");
                    suite.check(r.payload.size() == 8, "commit payload has commit_lsn");
                }
            }
            suite.check(saw_commit, "saw commit record");
        }
        detach(*database_id);
    }

    {
        auto opened = Database::open(temp.path());
        auto database = share(opened);
        suite.check(database != nullptr, "reopen");
        if (!database) {
            return suite.finish();
        }
        auto database_id = attach(database);
        suite.check(database_id.has_value(), "attach2");
        if (!database_id) {
            return suite.finish();
        }
        suite.check(database->bind(emp_builder()).has_value(), "rebind");
        suite.check(database->next_lsn() >= lsn_after_first, "lsn monotonic across reopen");
        auto tx = database->begin();
        suite.check(tx.has_value(), "begin2");
        if (tx) {
            suite.check(database->create(*tx, Emp{"bob"}).has_value(), "create bob");
            suite.check(tx->commit().has_value(), "commit2");
        }
        suite.check(database->next_lsn() > lsn_after_first, "lsn grew after second session");

        auto from = tx::Wal::read_from(temp.wal_path(), lsn_after_first);
        suite.check(from.has_value(), "read_from");
        if (from) {
            for (const auto& r : *from) {
                suite.check(r.lsn >= lsn_after_first, "read_from filters");
            }
        }

        auto repl = tx::Wal::read_for_replication(temp.wal_path(), 1);
        suite.check(repl.has_value(), "read_for_replication intact file");
        detach(*database_id);
    }

    {
        std::ofstream out(temp.wal_path(), std::ios::binary | std::ios::app);
        const char junk[] = {'x', 'y'};
        out.write(junk, 2);
        out.close();
        auto repl = tx::Wal::read_for_replication(temp.wal_path(), 1);
        suite.check(!repl.has_value(), "truncated WAL is error for replication");
        if (!repl) {
            suite.check(repl.error().code == ErrorCode::wal_corrupt, "wal_corrupt on truncate");
        }
    }

    return suite.finish();
}
