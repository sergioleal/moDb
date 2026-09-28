#include "modb/proxy/policy.hpp"

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

} // namespace modb::proxy
