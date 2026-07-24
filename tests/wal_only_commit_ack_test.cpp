#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
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
                ("modb-15c-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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
    TempPath path{"ack"};

    DatabaseOptions opts;
    opts.primary_storage = PrimaryStorage::wal_only;
    opts.commit_ack = CommitAckPolicy::local_wal;

    auto created = Database::create(path.path(), opts);
    suite.check(created.has_value(), "create");
    if (!created) {
        return suite.finish();
    }
    auto db = std::make_shared<Database>(std::move(*created));
    auto id = DatabaseRegistry::instance().attach(db);
    suite.check(db->bind(emp_builder()).has_value(), "bind");

    db->set_commit_ack_policy(CommitAckPolicy::await_one_replica, std::chrono::milliseconds{50});
    {
        auto tx = db->begin();
        suite.check(tx.has_value(), "begin without replica");
        if (tx) {
            suite.check(db->create(*tx, Emp{"x"}).has_value(), "create obj");
            auto committed = tx->commit();
            suite.check(!committed && (committed.error().code == ErrorCode::no_data_replica ||
                                       committed.error().code ==
                                           ErrorCode::commit_await_replica_timeout),
                        "commit without replica fails");
        }
    }

    // Pré-ACK alto: o await retorna assim que commit_lsn <= follower_ack_lsn.
    suite.check(db->set_follower_ack_lsn(1'000'000).has_value(), "pre-ack");
    db->set_commit_ack_policy(CommitAckPolicy::await_one_replica, std::chrono::seconds{2});
    {
        auto tx = db->begin();
        suite.check(tx.has_value(), "begin with pre-ack");
        if (!tx) {
            return suite.finish();
        }
        suite.check(db->create(*tx, Emp{"y"}).has_value(), "create obj2");
        auto committed = tx->commit();
        suite.check(committed.has_value(), "commit succeeds with pre-ACK");
    }

    db->set_commit_ack_policy(CommitAckPolicy::local_wal);
    ObjectId oid{};
    {
        auto tx = db->begin();
        suite.check(tx.has_value(), "begin local");
        if (!tx) {
            return suite.finish();
        }
        auto handle = db->create(*tx, Emp{"readable"});
        suite.check(handle.has_value() && tx->commit().has_value(), "local commit");
        if (handle) {
            oid = handle->id();
        }
    }
    auto got = db->get<Emp>(oid);
    suite.check(!got && got.error().code == ErrorCode::data_files_disabled,
                "get on wal_only primary rejected");

    if (id) {
        DatabaseRegistry::instance().detach(*id);
    }
    return suite.finish();
}
