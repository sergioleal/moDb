// Lesson 18 -- Protocol and Compatibility.
// Builds on Lesson 17's catalog discussion by practicing the version checks
// used before a client trusts a persisted artifact or network peer.
// See docs/training/en/18-protocol-and-compatibility/18-protocol-and-compatibility.md.

#include "modb/compatibility.hpp"

#include <iostream>

int main() {
    std::cout << "Objective: practice compatibility before crossing a process boundary.\n";

    const modb::CompatibilityVersion artifact{1, 2};
    const auto wire = modb::to_wire_u16(artifact);
    const auto decoded = modb::from_wire_u16(wire);
    std::cout << "Wire value for 1.2: " << wire << '\n';
    std::cout << "Decoded wire value: " << decoded.major << '.' << decoded.minor << '\n';

    auto negotiated = modb::negotiate_protocol_version(
        modb::CompatibilityVersion{1, 4}, modb::CompatibilityVersion{1, 2});
    if (!negotiated) {
        std::cerr << negotiated.error().message << '\n';
        return 1;
    }
    std::cout << "Negotiated protocol: " << negotiated->major << '.' << negotiated->minor
              << '\n';

    auto readable = modb::ensure_readable(modb::CompatibilityVersion{1, 3},
                                          modb::CompatibilityVersion{1, 2},
                                          modb::ErrorCode::incompatible_format_version,
                                          "training artifact");
    std::cout << "Readable check for artifact 1.3 with reader 1.2: "
              << (readable ? "accepted" : "rejected") << '\n';

    auto mismatch = modb::negotiate_protocol_version(modb::CompatibilityVersion{2, 0},
                                                     modb::CompatibilityVersion{1, 9});
    std::cout << "Protocol negotiation with major mismatch: "
              << (mismatch ? "accepted" : "rejected") << '\n';

    return (!readable && !mismatch) ? 0 : 1;
}
