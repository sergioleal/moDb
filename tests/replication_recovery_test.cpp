#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
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
        root_ = std::filesystem::temp_directory_path() / ("modb-14e-" + std::to_string(unique));
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
                    repl::install_bootstrap_snapshot(*snap, paths.follower_).has_value(),
                "bootstrap");

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

    auto follower_open = Database::open(paths.follower_);
    suite.check(follower_open.has_value(), "open follower");
    if (!follower_open) {
        return suite.finish();
    }
    auto follower = std::make_shared<Database>(std::move(*follower_open));
    auto fid = DatabaseRegistry::instance().attach(follower);
    follower->set_read_only_replica(true);

    auto rejected = follower->begin();
    suite.check(!rejected && rejected.error().code == ErrorCode::replica_read_only,
                "follower begin is read-only");

    // Catch-up completo, depois reconexão a partir de applied_lsn + 1.
    auto all = tx::Wal::read_from(primary->wal_path(), cut + 1);
    suite.check(all.has_value() && !all->empty(), "wal has post-cut records");
    if (!all || all->empty()) {
        return suite.finish();
    }

    auto applied1 = repl::apply_wal_records(follower->page_file(), *all, cut);
    suite.check(applied1.has_value() && *applied1 > cut, "catch-up apply");

    // Queda/reconexão: releitura a partir de applied+1 (sem novos commits → lote vazio).
    auto resume = tx::Wal::read_from(primary->wal_path(), *applied1 + 1);
    suite.check(resume.has_value(), "resume read from applied+1");
    auto applied2 =
        repl::apply_wal_records(follower->page_file(), *resume, *applied1);
    suite.check(applied2.has_value() && *applied2 == *applied1, "reconnect without new work");

    // Novo commit no primary + retoma.
    {
        auto tx = primary->begin();
        suite.check(primary->create(*tx, Emp{"d"}).has_value() && tx->commit().has_value(),
                    "commit4 after reconnect");
    }
    auto more = tx::Wal::read_from(primary->wal_path(), *applied1 + 1);
    suite.check(more.has_value() && !more->empty(), "new records after reconnect");
    auto applied3 =
        repl::apply_wal_records(follower->page_file(), *more, *applied1);
    suite.check(applied3.has_value() && *applied3 > *applied1, "reconnect apply advances");

    // Pedido abaixo da retenção → gap / rebootstrap.
    auto gap_frame =
        repl::build_wal_frame(primary->wal_path(), 0, primary->oldest_available_lsn() + 1);
    suite.check(!gap_frame && gap_frame.error().code == ErrorCode::replication_gap,
                "WalGap below retention");

    // UUID/timeline do follower batem com o primary após bootstrap.
    suite.check(follower->database_uuid() == primary->database_uuid() &&
                    follower->timeline_id() == primary->timeline_id(),
                "identity matches after bootstrap");

    if (fid) {
        DatabaseRegistry::instance().detach(*fid);
    }
    if (pid) {
        DatabaseRegistry::instance().detach(*pid);
    }
    return suite.finish();
}
