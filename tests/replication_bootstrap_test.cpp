#include "modb/object/database.hpp"
#include "modb/object/binding.hpp"
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
        root_ = std::filesystem::temp_directory_path() / ("modb-14c-" + std::to_string(unique));
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
    auto id = DatabaseRegistry::instance().attach(primary);
    suite.check(id.has_value(), "attach");
    suite.check(primary->bind(emp_builder()).has_value(), "bind");
    {
        auto tx = primary->begin();
        suite.check(tx.has_value(), "begin");
        suite.check(primary->create(*tx, Emp{"seed"}).has_value(), "seed");
        suite.check(tx->commit().has_value(), "commit");
    }

    auto snap = repl::create_bootstrap_snapshot(*primary, paths.root_ / "tmp");
    suite.check(snap.has_value(), "bootstrap snapshot");
    if (!snap) {
        return suite.finish();
    }
    suite.check(snap->begin.cut_lsn > 0, "cut_lsn set");
    suite.check(repl::install_bootstrap_snapshot(*snap, paths.follower_).has_value(), "install");

    auto opened = Database::open(paths.follower_);
    suite.check(opened.has_value(), "open follower copy");
    if (opened) {
        auto follower = std::make_shared<Database>(std::move(*opened));
        auto fid = DatabaseRegistry::instance().attach(follower);
        suite.check(follower->bind(emp_builder()).has_value(), "follower bind");
        suite.check(follower->database_uuid() == primary->database_uuid(), "uuid match");
        follower->set_read_only_replica(true);
        auto denied = follower->begin();
        suite.check(!denied && denied.error().code == ErrorCode::replica_read_only,
                    "follower begin rejected");
        if (fid) {
            DatabaseRegistry::instance().detach(*fid);
        }
    }

    DatabaseRegistry::instance().detach(*id);
    return suite.finish();
}
