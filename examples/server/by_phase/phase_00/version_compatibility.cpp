#include "modb/compatibility.hpp"
#include "modb/version.hpp"

#include <iostream>

int main() {
    std::cout << "Objective: show project version and protocol compatibility negotiation.\n";

    // Phase 0 exposes the public version contract used by clients and servers.
    const modb::CompatibilityVersion client{1, 0};
    const modb::CompatibilityVersion server{1, 0};

    // Negotiation returns the protocol version both sides can safely use.
    auto negotiated = modb::negotiate_protocol_version(client, server);
    if (!negotiated) {
        std::cerr << negotiated.error().message << '\n';
        return 1;
    }

    std::cout << modb::project_name() << ' ' << modb::project_version() << '\n';
    std::cout << "protocol " << negotiated->major << '.' << negotiated->minor << '\n';
    return 0;
}
