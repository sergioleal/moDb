#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
#include "modb/object/instance_control.hpp"
#include "modb/tx/wal.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
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

class TempPath {
public:
    explicit TempPath(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-15b-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TempPath() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + ".wal", ignored);
        std::filesystem::remove(path_.string() + ".scratch", ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

int main() {
    TestSuite suite;
    TempPath path{"primary"};

    DatabaseOptions opts;
    opts.primary_storage = PrimaryStorage::wal_only;
    opts.commit_ack = CommitAckPolicy::local_wal;

    DatabaseUuid uuid{};
    std::uint64_t after_commit = 0;
    {
        auto created = Database::create(path.path(), opts);
        suite.check(created.has_value(), "create wal_only");
        if (!created) {
            return suite.finish();
        }
        auto db = std::make_shared<Database>(std::move(*created));
        auto id = DatabaseRegistry::instance().attach(db);
        suite.check(db->bind(emp_builder()).has_value(), "bind");
        {
            auto tx = db->begin();
            suite.check(db->create(*tx, Emp{"a"}).has_value() && tx->commit().has_value(),
                        "commit writes WAL");
        }
        uuid = db->database_uuid();
        after_commit = db->checkpoint_lsn();
        suite.check(is_instance_control_file(path.path()), "control file MCTL");
        suite.check(std::filesystem::exists(db->wal_path()), "wal exists");
        // Arquivo de dados no path lógico não é PageFile: é MCTL.
        auto as_full = Database::open(path.path());
        suite.check(!as_full, "logical path is not a full PageFile");
        if (id) {
            DatabaseRegistry::instance().detach(*id);
        }
    }

    {
        auto reopened = Database::open(path.path(), opts);
        suite.check(reopened.has_value(), "reopen wal_only");
        if (reopened) {
            suite.check(reopened->database_uuid() == uuid, "uuid restored");
            suite.check(reopened->checkpoint_lsn() >= after_commit, "checkpoint restored");
            suite.check(reopened->next_lsn() > after_commit, "next_lsn restored");
            auto records = tx::Wal::read_all(reopened->wal_path());
            suite.check(records.has_value() && !records->empty(), "wal durable across reopen");
        }
    }

    return suite.finish();
}
