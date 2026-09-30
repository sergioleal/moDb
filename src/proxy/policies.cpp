#include "modb/proxy/policies.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace modb::proxy {
namespace {

std::string_view trim(std::string_view s) {
    constexpr std::string_view blanks = " \t\r\n";
    const auto first = s.find_first_not_of(blanks);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(blanks) - first + 1);
}

std::vector<std::string_view> words_of(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        const auto start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (i > start) {
            out.push_back(line.substr(start, i - start));
        }
    }
    return out;
}

// Exato, prefixo com `*` no fim, ou `*`.
[[nodiscard]] bool matches(std::string_view pattern, std::string_view value) noexcept {
    if (pattern == "*") {
        return true;
    }
    if (pattern.ends_with('*')) {
        return value.starts_with(pattern.substr(0, pattern.size() - 1));
    }
    return pattern == value;
}

// O que o pedido é, para as regras: tipo e alvo.
struct Subject {
    std::string_view kind;
    std::string target;
};

[[nodiscard]] std::optional<Subject> subject_of(const net::Message& request) {
    if (const auto* call = std::get_if<net::OpCall>(&request)) {
        return Subject{"call", call->operation_id};
    }
    if (const auto* query = std::get_if<net::Query>(&request)) {
        return Subject{"query", std::to_string(query->description.type.value)};
    }
    if (const auto* open = std::get_if<net::FacadeOpen>(&request)) {
        return Subject{"facade", open->facade_id};
    }
    if (std::holds_alternative<net::FacadeList>(request)) {
        return Subject{"facade", "*"};
    }
    return std::nullopt;
}

} // namespace

// --- PolicyChain --------------------------------------------------------------

PolicyChain::PolicyChain(std::vector<std::shared_ptr<Policy>> links) : links_{std::move(links)} {
    std::erase(links_, nullptr);
    for (const auto& link : links_) {
        name_ += (name_.empty() ? "" : "+") + std::string{link->name()};
    }
    if (name_.empty()) {
        name_ = "passthrough";
    }
}

std::vector<std::string> PolicyChain::mechanisms() const {
    for (const auto& link : links_) {
        if (auto mechanisms = link->mechanisms(); !mechanisms.empty()) {
            return mechanisms;
        }
    }
    return {};
}

Result<ops::Caller> PolicyChain::authenticate(const ClientInfo& client, const Credentials& credentials) {
    for (const auto& link : links_) {
        if (!link->mechanisms().empty()) {
            return link->authenticate(client, credentials);
        }
    }
    return Policy::authenticate(client, credentials);
}

Decision PolicyChain::authorize(const ops::Caller& caller, net::Message& request) {
    for (const auto& link : links_) {
        if (auto decision = link->authorize(caller, request); !decision.allowed) {
            return decision;
        }
    }
    return Decision::allow();
}

void PolicyChain::on_response(const ops::Caller& caller, net::Message& response) {
    for (const auto& link : links_) {
        link->on_response(caller, response);
    }
}

void PolicyChain::audit(const AuditRecord& record) {
    for (const auto& link : links_) {
        link->audit(record);
    }
}

void PolicyChain::on_engine(const EngineInfo& engine) {
    for (const auto& link : links_) {
        link->on_engine(engine);
    }
}

// --- ReadOnlyPolicy -----------------------------------------------------------

Decision ReadOnlyPolicy::authorize(const ops::Caller& /*caller*/, net::Message& request) {
    const auto* call = std::get_if<net::OpCall>(&request);
    if (call == nullptr) {
        return Decision::allow();
    }
    std::optional<bool> read_only;
    {
        const std::shared_lock lock{mu_};
        read_only = engine_.read_only(call->operation_id);
    }
    if (!read_only) {
        return Decision::deny("read-only proxy: unknown operation " + call->operation_id);
    }
    if (!*read_only) {
        return Decision::deny("read-only proxy: " + call->operation_id + " writes");
    }
    return Decision::allow();
}

void ReadOnlyPolicy::on_engine(const EngineInfo& engine) {
    const std::unique_lock lock{mu_};
    engine_ = engine;
}

// --- AllowlistPolicy ----------------------------------------------------------

Result<std::shared_ptr<AllowlistPolicy>> AllowlistPolicy::parse(std::string_view text) {
    std::vector<Rule> rules;
    std::size_t number = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        start = end == std::string_view::npos ? text.size() : end + 1;
        ++number;
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) {
            continue;
        }
        const auto words = words_of(line);
        const auto where = "allowlist line " + std::to_string(number) + ": ";
        if (words.size() != 3) {
            return std::unexpected(Error{ErrorCode::invalid_argument, where + "expected '<who> <kind> <target>'"});
        }
        const auto kind = words[1];
        if (kind != "call" && kind != "query" && kind != "facade" && kind != "*") {
            return std::unexpected(Error{ErrorCode::invalid_argument,
                                         where + "kind must be call, query, facade or *"});
        }
        if (words[0] == "user:") {
            return std::unexpected(Error{ErrorCode::invalid_argument, where + "user: needs a principal"});
        }
        rules.push_back(Rule{std::string{words[0]}, std::string{kind}, std::string{words[2]}});
    }
    return std::make_shared<AllowlistPolicy>(std::move(rules));
}

Result<std::shared_ptr<AllowlistPolicy>> AllowlistPolicy::load(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return std::unexpected(Error{ErrorCode::file_not_found, "cannot read allowlist file: " + file.string()});
    }
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    return parse(text);
}

Decision AllowlistPolicy::authorize(const ops::Caller& caller, net::Message& request) {
    const auto subject = subject_of(request);
    if (!subject) {
        return Decision::allow();  // ShmAttach e afins: o proxy decide
    }
    for (const auto& rule : rules_) {
        const bool who = rule.who == "*" ||
                         (rule.who.starts_with("user:") && !caller.anonymous() &&
                          std::string_view{rule.who}.substr(5) == caller.principal) ||
                         (!rule.who.starts_with("user:") && caller.has_role(rule.who));
        if (who && (rule.kind == "*" || rule.kind == subject->kind) && matches(rule.target, subject->target)) {
            return Decision::allow();
        }
    }
    return Decision::deny("not allowed: " + std::string{subject->kind} + " " + subject->target);
}

// --- AuditLogPolicy -----------------------------------------------------------

std::shared_ptr<AuditLogPolicy> AuditLogPolicy::to_stream(std::ostream& out) {
    auto mu = std::make_shared<std::mutex>();
    return std::make_shared<AuditLogPolicy>([&out, mu](std::string_view line) {
        const std::scoped_lock lock{*mu};
        out << line << '\n';
        out.flush();
    });
}

std::string AuditLogPolicy::format(const AuditRecord& record) {
    char ms[32];
    std::snprintf(ms, sizeof(ms), "%.3fms", std::chrono::duration<double, std::milli>(record.duration).count());
    std::string line = "audit " + std::string{record.kind} + " " +
                       (record.target.empty() ? std::string{"-"} : std::string{record.target}) + " ";
    line += record.ok ? std::string{"ok"} : "error " + std::to_string(static_cast<unsigned>(record.code));
    line += " " + std::string{ms};
    if (record.denied) {
        line += " denied";
    }
    if (record.kind == "query") {
        line += " objects " + std::to_string(record.objects);
    }
    const bool anonymous = record.caller == nullptr || record.caller->anonymous();
    line += " by " + (anonymous ? std::string{"-"} : record.caller->principal);
    if (!anonymous && record.caller->delegated()) {
        line += " as " + record.caller->acting_as;
    }
    line += " from " + (record.client.empty() ? std::string{"-"} : std::string{record.client});
    return line;
}

void AuditLogPolicy::audit(const AuditRecord& record) {
    if (sink_) {
        sink_(format(record));
    }
}

// --- RateLimitPolicy ----------------------------------------------------------

RateLimitPolicy::RateLimitPolicy(std::uint32_t calls_per_second, std::uint32_t streams_per_principal,
                                 std::function<Clock::time_point()> now, std::uint32_t delegated_calls_per_second)
    : calls_per_second_{calls_per_second}, delegated_calls_per_second_{delegated_calls_per_second},
      streams_per_principal_{streams_per_principal}, now_{std::move(now)} {}

std::string RateLimitPolicy::key_of(const ops::Caller& caller) {
    if (caller.delegated()) {
        // Cada delegado tem o seu balde, dentro do principal que fala por ele.
        return "user:" + caller.principal + " as " + caller.acting_as;
    }
    if (!caller.anonymous()) {
        return "user:" + caller.principal;
    }
    // Anônimo: pela máquina, sem a porta (que muda a cada conexão).
    const auto address = caller.attribute("address");
    return "host:" + std::string{address.substr(0, address.rfind(':'))};
}

Decision RateLimitPolicy::authorize(const ops::Caller& caller, net::Message& request) {
    const bool call = std::holds_alternative<net::OpCall>(request);
    const bool query = std::holds_alternative<net::Query>(request);
    if (!call && !query) {
        return Decision::allow();
    }
    const std::scoped_lock lock{mu_};
    auto& account = accounts_[key_of(caller)];
    if (call && (calls_per_second_ > 0 || (caller.delegated() && delegated_calls_per_second_ > 0))) {
        const auto now = now_();
        if (calls_per_second_ > 0 && !take(account, calls_per_second_, now)) {
            return Decision::deny("rate limit: more than " + std::to_string(calls_per_second_) + " calls per second");
        }
        // A soma dos delegados de um principal.
        if (caller.delegated() && delegated_calls_per_second_ > 0 &&
            !take(accounts_["delegates:" + caller.principal], delegated_calls_per_second_, now)) {
            return Decision::deny("rate limit: more than " + std::to_string(delegated_calls_per_second_) +
                                  " delegated calls per second for " + caller.principal);
        }
    }
    if (query && streams_per_principal_ > 0) {
        if (account.streams >= streams_per_principal_) {
            return Decision::deny("limit: " + std::to_string(streams_per_principal_) + " open streams");
        }
        ++account.streams;
    }
    return Decision::allow();
}

bool RateLimitPolicy::take(Account& account, std::uint32_t per_second, Clock::time_point now) {
    if (account.refilled == Clock::time_point{}) {
        account.tokens = per_second;
    } else {
        const double elapsed = std::chrono::duration<double>(now - account.refilled).count();
        account.tokens = std::min<double>(per_second, account.tokens + elapsed * per_second);
    }
    account.refilled = now;
    if (account.tokens < 1.0) {
        return false;
    }
    account.tokens -= 1.0;
    return true;
}

void RateLimitPolicy::audit(const AuditRecord& record) {
    // Uma consulta que foi ao engine terminou (fim, erro ou cliente saiu).
    if (record.kind != "query" || record.denied || streams_per_principal_ == 0 || record.caller == nullptr) {
        return;
    }
    const std::scoped_lock lock{mu_};
    if (auto found = accounts_.find(key_of(*record.caller)); found != accounts_.end() && found->second.streams > 0) {
        --found->second.streams;
    }
}

std::uint32_t RateLimitPolicy::open_streams(const ops::Caller& caller) const {
    const std::scoped_lock lock{mu_};
    const auto found = accounts_.find(key_of(caller));
    return found == accounts_.end() ? 0 : found->second.streams;
}

} // namespace modb::proxy
