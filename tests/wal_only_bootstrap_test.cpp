#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
#include "modb/repl/replication.hpp"
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

class TempPaths {
public:
    TempPaths() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() / ("modb-15d-" + std::to_string(unique));
        std::filesystem::create_directories(root_);
        primary_ = root_ / "primary.modb";
        follower_ = root_ / "follower.modb";
        donor_ = root_ / "donor.modb";
    }
    ~TempPaths() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    std::filesystem::path root_;
    std::filesystem::path primary_;
    std::filesystem::path follower_;
    std::filesystem::path donor_;
};

} // namespace

int main() {
    TestSuite suite;
    TempPaths paths;

    DatabaseOptions opts;
    opts.primary_storage = PrimaryStorage::wal_only;
    opts.commit_ack = CommitAckPolicy::local_wal;

    auto created = Database::create(paths.primary_, opts);
    suite.check(created.has_value(), "create wal_only primary");
    if (!created) {
        return suite.finish();
    }
    auto primary = std::make_shared<Database>(std::move(*created));
    auto pid = DatabaseRegistry::instance().attach(primary);
    suite.check(primary->bind(emp_builder()).has_value(), "bind");
    {
        auto tx = primary->begin();
        suite.check(primary->create(*tx, Emp{"seed"}).has_value() && tx->commit().has_value(),
                    "commit");
    }

    auto refused = repl::create_bootstrap_snapshot(*primary, paths.root_ / "tmp");
    suite.check(!refused && refused.error().code == ErrorCode::data_files_disabled,
                "wal_only cannot donate snapshot");

    auto applied = repl::seed_replica_from_wal(paths.follower_, primary->wal_path(),
                                               primary->database_uuid(), primary->timeline_id(), 1);
    suite.check(applied.has_value() && *applied > 0, "seed from empty+WAL");

    auto follower_open = Database::open(paths.follower_);
    suite.check(follower_open.has_value(), "open seeded follower");
    if (follower_open) {
        suite.check(follower_open->database_uuid() == primary->database_uuid(), "uuid match");
        suite.check(follower_open->has_durable_data_files(), "follower has data files");
    }

    // Doação entre réplicas de dados (fonte ≠ primary wal_only).
    if (follower_open) {
        auto donor = std::make_shared<Database>(std::move(*follower_open));
        auto did = DatabaseRegistry::instance().attach(donor);
        auto snap = repl::create_bootstrap_snapshot(*donor, paths.root_ / "donate-tmp");
        suite.check(snap.has_value(), "donor replica can snapshot");
        if (snap) {
            suite.check(repl::install_bootstrap_snapshot(*snap, paths.donor_).has_value(),
                        "install donated snapshot");
        }
        if (did) {
            DatabaseRegistry::instance().detach(*did);
        }
    }

    if (pid) {
        DatabaseRegistry::instance().detach(*pid);
    }
    return suite.finish();
}
