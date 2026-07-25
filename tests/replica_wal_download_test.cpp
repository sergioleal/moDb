#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"
#include "modb/repl/wal_downloader.hpp"
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

class TempPaths {
public:
    TempPaths() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() / ("modb-16c-" + std::to_string(unique));
        std::filesystem::create_directories(root_);
        primary_ = root_ / "primary.modb";
        spool_ = root_ / "spool";
    }
    ~TempPaths() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    std::filesystem::path root_;
    std::filesystem::path primary_;
    std::filesystem::path spool_;
};

} // namespace

int main() {
    TestSuite suite;
    TempPaths paths;

    auto created = Database::create(paths.primary_);
    suite.check(created.has_value(), "create primary");
    auto primary = std::make_shared<Database>(std::move(*created));
    auto pid = DatabaseRegistry::instance().attach(primary);
    suite.check(primary->bind(emp_builder()).has_value(), "bind");
    {
        auto tx = primary->begin();
        suite.check(primary->create(*tx, Emp{"a"}).has_value() && tx->commit().has_value(),
                    "commit");
    }
    auto manifest = repl::build_wal_manifest(primary->wal_path(), primary->database_uuid(),
                                             primary->timeline_id(), 1);
    suite.check(manifest.has_value(), "manifest");
    auto downloaded = repl::download_wal_segments(*manifest, repl::WalDownloadOptions{paths.spool_});
    suite.check(downloaded.has_value() && downloaded->segment_paths.size() == 1,
                "downloaded one segment");
    if (downloaded && !downloaded->segment_paths.empty()) {
        suite.check(std::filesystem::exists(downloaded->segment_paths.front()),
                    "segment exists in spool");
        auto resumed =
            repl::download_wal_segments(*manifest, repl::WalDownloadOptions{paths.spool_});
        suite.check(resumed.has_value() && resumed->segment_paths == downloaded->segment_paths,
                    "resume reuses validated segment");
    }

    auto bad = *manifest;
    bad.segments.front().hash = "fnv1a64:0000000000000000";
    auto rejected = repl::download_wal_segments(bad, repl::WalDownloadOptions{paths.root_ / "bad"});
    suite.check(!rejected && rejected.error().code == ErrorCode::manifest_hash_mismatch,
                "hash mismatch rejected");

    if (pid) {
        DatabaseRegistry::instance().detach(*pid);
    }
    return suite.finish();
}
