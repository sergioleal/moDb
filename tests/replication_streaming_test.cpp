#include "modb/net/replication_protocol.hpp"
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
        root_ = std::filesystem::temp_directory_path() / ("modb-14d-stream-" + std::to_string(unique));
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
        suite.check(primary->create(*tx, Emp{"stream"}).has_value() && tx->commit().has_value(),
                    "commit");
    }

    const auto from_lsn = primary->oldest_available_lsn();
    auto frame = repl::build_wal_frame(primary->wal_path(), from_lsn,
                                       primary->oldest_available_lsn());
    suite.check(frame.has_value() && frame->first_lsn >= from_lsn && !frame->records.empty(),
                "build wal frame");

    if (frame) {
        auto encoded = net::encode_replication_message(*frame);
        suite.check(encoded.has_value(), "encode frame for wire");
        if (encoded) {
            auto decoded = net::decode_replication_message(*encoded);
            suite.check(decoded.has_value(), "decode frame");
            if (decoded) {
                auto* out = std::get_if<net::WalFrame>(&*decoded);
                suite.check(out != nullptr && out->crc == frame->crc &&
                                out->records.size() == frame->records.size(),
                            "frame wire round-trip");
            }
        }

        net::WalSubscribe sub;
        sub.database_uuid = primary->database_uuid();
        sub.timeline_id = primary->timeline_id();
        sub.from_lsn = from_lsn;
        auto enc_sub = net::encode_replication_message(sub);
        suite.check(enc_sub.has_value(), "encode subscribe");

        net::ReplicationHeartbeat hb;
        hb.primary_commit_lsn = primary->checkpoint_lsn();
        auto enc_hb = net::encode_replication_message(hb);
        suite.check(enc_hb.has_value() && net::decode_replication_message(*enc_hb).has_value(),
                    "heartbeat round-trip");

        net::WalAck ack;
        ack.applied_lsn = frame->last_lsn;
        suite.check(primary->set_follower_ack_lsn(ack.applied_lsn).has_value() &&
                        primary->follower_ack_lsn() == ack.applied_lsn,
                    "ack advances follower_ack_lsn");
    }

    // Below retention → gap (backpressure path for rebootstrap).
    auto below = repl::build_wal_frame(primary->wal_path(), 0, primary->oldest_available_lsn() + 10);
    suite.check(!below && below.error().code == ErrorCode::replication_gap,
                "subscribe below retention is gap");

    if (pid) {
        DatabaseRegistry::instance().detach(*pid);
    }
    return suite.finish();
}
