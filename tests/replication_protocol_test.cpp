#include "modb/net/replication_protocol.hpp"
#include "test_support.hpp"

using namespace modb;
using namespace modb::net;
using namespace modb::object;

int main() {
    TestSuite suite;

    ReplicationHello hello;
    hello.version = 1;
    hello.database_uuid.bytes[0] = 0xAB;
    hello.timeline_id = TimelineId{7};
    auto encoded = encode_replication_message(hello);
    suite.check(encoded.has_value(), "encode hello");
    if (encoded) {
        auto decoded = decode_replication_message(*encoded);
        suite.check(decoded.has_value(), "decode hello");
        if (decoded) {
            auto* out = std::get_if<ReplicationHello>(&*decoded);
            suite.check(out != nullptr && *out == hello, "hello round-trip");
        }
    }

    WalFrame frame;
    frame.first_lsn = 10;
    frame.last_lsn = 12;
    frame.records = {std::byte{1}, std::byte{2}, std::byte{3}};
    auto enc_frame = encode_replication_message(frame);
    suite.check(enc_frame.has_value(), "encode frame");
    if (enc_frame) {
        auto decoded = decode_replication_message(*enc_frame);
        suite.check(decoded.has_value(), "decode frame");
        if (decoded) {
            auto* out = std::get_if<WalFrame>(&*decoded);
            suite.check(out != nullptr && out->first_lsn == 10 && out->records.size() == 3,
                        "frame round-trip");
        }
    }

    WalGap gap;
    gap.oldest_available_lsn = 5;
    auto enc_gap = encode_replication_message(gap);
    suite.check(enc_gap.has_value() && decode_replication_message(*enc_gap).has_value(),
                "gap round-trip");

    return suite.finish();
}
