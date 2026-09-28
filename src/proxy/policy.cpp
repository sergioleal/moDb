#include "modb/proxy/policy.hpp"

#include <algorithm>

namespace modb::proxy {

Result<ops::Caller> Policy::authenticate(const ClientInfo& client, const Credentials& /*credentials*/) {
    ops::Caller caller;
    caller.attributes.emplace_back("address", client.address);
    caller.attributes.emplace_back("tls", client.tls ? "1" : "0");
    return caller;
}

Decision Policy::authorize(const ops::Caller& /*caller*/, net::Message& /*request*/) { return Decision::allow(); }

void Policy::on_response(const ops::Caller& /*caller*/, net::Message& /*response*/) {}

void Policy::audit(const AuditRecord& /*record*/) {}

void Policy::on_engine(const EngineInfo& /*engine*/) {}

std::optional<bool> EngineInfo::read_only(std::string_view id) const {
    const auto found = std::lower_bound(operations.begin(), operations.end(), id,
                                        [](const Operation& op, std::string_view key) { return op.id < key; });
    if (found == operations.end() || found->id != id) {
        return std::nullopt;
    }
    return found->read_only;
}

} // namespace modb::proxy
