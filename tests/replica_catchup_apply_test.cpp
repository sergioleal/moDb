#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
#include "modb/repl/replication.hpp"
#include "modb/repl/wal_downloader.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
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
        root_ = std::filesystem::temp_directory_path() / ("modb-16d-" + std::to_string(unique));
        std::filesystem::create_directories(root_);
        primary_ = root_ / "primary.modb";
        empty_follower_ = root_ / "empty-follower.modb";
        partial_follower_ = root_ / "partial-follower.modb";
    }
    ~TempPaths() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    std::filesystem::path root_;
    std::filesystem::path primary_;
    std::filesystem::path empty_follower_;
    std::filesystem::path partial_follower_;
};

} // namespace

int main() {
    TestSuite suite;
    TempPaths paths;

    auto created = Database::create(paths.primary_);
    suite.check(created.has_value(), "create primary");
    if (!created) {
        return suite.finish();
    }
    auto primary = std::make_shared<Database>(std::move(*created));
    auto pid = DatabaseRegistry::instance().attach(primary);
    suite.check(primary->bind(emp_builder()).has_value(), "bind");
    {
        auto tx = primary->begin();
        suite.check(primary->create(*tx, Emp{"a"}).has_value() && tx->commit().has_value(),
                    "commit1");
    }
    const auto cut = primary->checkpoint_lsn();
    auto snap = repl::create_bootstrap_snapshot(*primary, paths.root_ / "tmp");
    suite.check(snap.has_value() &&
                    repl::install_bootstrap_snapshot(*snap, paths.partial_follower_).has_value(),
                "partial follower bootstrap");
    {
        auto tx = primary->begin();
        suite.check(primary->create(*tx, Emp{"b"}).has_value() && tx->commit().has_value(),
                    "commit2");
    }
    {
        auto tx = primary->begin();
        suite.check(primary->create(*tx, Emp{"c"}).has_value() && tx->commit().has_value(),
                    "commit3");
    }

    repl::ReplicaCatchupOptions empty;
    empty.replica_path = paths.empty_follower_;
    empty.wal_source = primary->wal_path();
    empty.spool_dir = paths.root_ / "spool-empty";
    empty.database_uuid = primary->database_uuid();
    empty.timeline = primary->timeline_id();
    empty.oldest_available_lsn = 1;
    auto empty_result = repl::catch_up_replica_from_wal(empty);
    if (!empty_result) {
        std::cerr << "empty catch-up error: " << empty_result.error().message << '\n';
    }
    suite.check(empty_result.has_value() && empty_result->state == repl::CatchupState::up_to_date,
                "empty replica reaches up_to_date");

    auto empty_meta = repl::read_catchup_metadata(paths.empty_follower_);
    suite.check(empty_result && empty_meta.has_value() &&
                    empty_meta->state == repl::CatchupState::up_to_date &&
                    empty_meta->applied_lsn == empty_result->applied_lsn,
                "empty replica metadata persisted");

    repl::ReplicaCatchupOptions partial;
    partial.replica_path = paths.partial_follower_;
    partial.wal_source = primary->wal_path();
    partial.spool_dir = paths.root_ / "spool-partial";
    partial.database_uuid = primary->database_uuid();
    partial.timeline = primary->timeline_id();
    partial.oldest_available_lsn = cut + 1;
    auto partial_result = repl::catch_up_replica_from_wal(partial);
    if (!partial_result) {
        std::cerr << "partial catch-up error: " << partial_result.error().message << '\n';
    }
    suite.check(empty_result && partial_result.has_value() &&
                    partial_result->applied_lsn == empty_result->applied_lsn,
                "partial replica catches up from discovered checkpoint");

    repl::ReplicaCatchupOptions gap = partial;
    gap.replica_path = paths.root_ / "gap-follower.modb";
    gap.oldest_available_lsn = 50;
    auto rejected = repl::catch_up_replica_from_wal(gap);
    suite.check(!rejected && rejected.error().code == ErrorCode::bootstrap_required,
                "gap beyond retention requires bootstrap");

    if (pid) {
        DatabaseRegistry::instance().detach(*pid);
    }
    return suite.finish();
}
