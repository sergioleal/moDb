#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
#include "modb/repl/wal_manifest.hpp"
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
        root_ = std::filesystem::temp_directory_path() / ("modb-16b-" + std::to_string(unique));
        std::filesystem::create_directories(root_);
        primary_ = root_ / "primary.modb";
    }
    ~TempPaths() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    std::filesystem::path root_;
    std::filesystem::path primary_;
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
        suite.check(tx.has_value(), "begin");
        if (tx) {
            suite.check(primary->create(*tx, Emp{"a"}).has_value() && tx->commit().has_value(),
                        "commit");
        }
    }

    auto manifest = repl::build_wal_manifest(primary->wal_path(), primary->database_uuid(),
                                             primary->timeline_id(), 1);
    suite.check(manifest.has_value() && !manifest->segments.empty(), "manifest built");
    if (manifest) {
        suite.check(manifest->first_lsn == 1 && manifest->last_lsn >= manifest->first_lsn,
                    "manifest range");
        suite.check(repl::validate_wal_manifest(*manifest, primary->database_uuid(),
                                                primary->timeline_id(), 0)
                        .has_value(),
                    "manifest validates");

        auto bad_hash = *manifest;
        bad_hash.segments.front().hash = "";
        auto rejected = repl::validate_wal_manifest(bad_hash, primary->database_uuid(),
                                                    primary->timeline_id(), 0);
        suite.check(!rejected && rejected.error().code == ErrorCode::invalid_encoding,
                    "missing hash rejected");

        auto gap = *manifest;
        gap.first_lsn = 10;
        gap.oldest_available_lsn = 10;
        gap.segments.front().first_lsn = 10;
        auto needs_bootstrap =
            repl::validate_wal_manifest(gap, primary->database_uuid(), primary->timeline_id(), 0);
        suite.check(!needs_bootstrap &&
                        needs_bootstrap.error().code == ErrorCode::bootstrap_required,
                    "gap requires bootstrap");
    }

    if (pid) {
        DatabaseRegistry::instance().detach(*pid);
    }
    return suite.finish();
}
