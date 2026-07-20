#include "modb/object/database.hpp"
#include "modb/object/binding.hpp"
#include "modb/repl/replication.hpp"
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

class TempPaths {
public:
    TempPaths() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() / ("modb-14d-" + std::to_string(unique));
        std::filesystem::create_directories(root_);
        primary_ = root_ / "primary.modb";
        follower_ = root_ / "follower.modb";
    }
    ~TempPaths() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    std::filesystem::path root_;
    std::filesystem::path primary_;
    std::filesystem::path follower_;
};

} // namespace

int main() {
    TestSuite suite;
    TempPaths paths;

    auto created = Database::create(paths.primary_);
    auto primary = std::make_shared<Database>(std::move(*created));
    auto pid = DatabaseRegistry::instance().attach(primary);
    suite.check(primary->bind(emp_builder()).has_value(), "bind");
    {
        auto tx = primary->begin();
        suite.check(primary->create(*tx, Emp{"a"}).has_value() && tx->commit().has_value(),
                    "commit1");
    }
    const auto after_first = primary->checkpoint_lsn();

    auto snap = repl::create_bootstrap_snapshot(*primary, paths.root_ / "tmp");
    suite.check(snap.has_value() &&
                    repl::install_bootstrap_snapshot(*snap, paths.follower_).has_value(),
                "bootstrap");

    {
        auto tx = primary->begin();
        suite.check(primary->create(*tx, Emp{"b"}).has_value() && tx->commit().has_value(),
                    "commit2");
    }

    auto records = tx::Wal::read_from(primary->wal_path(), after_first + 1);
    suite.check(records.has_value() && !records->empty(), "wal records after cut");

    auto follower_open = Database::open(paths.follower_);
    suite.check(follower_open.has_value(), "open follower");
    if (!follower_open) {
        return suite.finish();
    }
    auto follower = std::make_shared<Database>(std::move(*follower_open));
    auto fid = DatabaseRegistry::instance().attach(follower);
    follower->set_read_only_replica(true);

    auto applied =
        repl::apply_wal_records(follower->page_file(), *records, snap->begin.cut_lsn);
    suite.check(applied.has_value() && *applied > snap->begin.cut_lsn, "apply advances");

    // Reaplicar o mesmo lote é idempotente.
    auto again =
        repl::apply_wal_records(follower->page_file(), *records, snap->begin.cut_lsn);
    suite.check(again.has_value() && *again == *applied, "apply idempotent");

    // Gap: pede LSN futuro.
    std::vector<tx::WalRecord> gapped = *records;
    if (!gapped.empty()) {
        gapped[0].lsn += 100;
        auto gap = repl::apply_wal_records(follower->page_file(), gapped, *applied);
        suite.check(!gap && gap.error().code == ErrorCode::replication_gap, "gap detected");
    }

    if (fid) {
        DatabaseRegistry::instance().detach(*fid);
    }
    DatabaseRegistry::instance().detach(*pid);
    return suite.finish();
}
