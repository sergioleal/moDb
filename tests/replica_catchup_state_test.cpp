#include "modb/repl/wal_manifest.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <system_error>

using namespace modb;

namespace {

class TempPaths {
public:
    TempPaths() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() / ("modb-16a-" + std::to_string(unique));
        std::filesystem::create_directories(root_);
        replica_ = root_ / "replica.modb";
    }
    ~TempPaths() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    std::filesystem::path root_;
    std::filesystem::path replica_;
};

} // namespace

int main() {
    TestSuite suite;
    TempPaths paths;

    auto missing = repl::read_catchup_metadata(paths.replica_);
    suite.check(missing.has_value() && missing->state == repl::CatchupState::empty,
                "missing sidecar means empty");

    repl::CatchupMetadata metadata{repl::CatchupState::catching_up, 7, 11, "retry"};
    suite.check(repl::write_catchup_metadata(paths.replica_, metadata).has_value(),
                "write sidecar");
    auto read = repl::read_catchup_metadata(paths.replica_);
    suite.check(read.has_value() && *read == metadata, "metadata round-trip");

    suite.check(repl::parse_catchup_state("up_to_date").value() ==
                    repl::CatchupState::up_to_date,
                "parse state");
    suite.check(repl::validate_catchup_transition(repl::CatchupState::empty,
                                                  repl::CatchupState::catching_up)
                    .has_value(),
                "empty can catch up");
    suite.check(repl::validate_catchup_transition(repl::CatchupState::up_to_date,
                                                  repl::CatchupState::catching_up)
                    .has_value(),
                "up_to_date can catch up again when new WAL appears");
    auto invalid = repl::validate_catchup_transition(repl::CatchupState::up_to_date,
                                                    repl::CatchupState::seeding);
    suite.check(!invalid && invalid.error().code == ErrorCode::invalid_replica_state,
                "invalid transition rejected");

    return suite.finish();
}
