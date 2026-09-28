// Políticas de referência do proxy (ADR-028, PLANO_PROXY X7): read_only pelo
// catálogo do engine, allowlist por role/usuário, linha de auditoria, limites
// por principal (relógio falso) e a composição; depois, de ponta a ponta, uma
// cadeia token → allowlist → read_only → limites → auditoria na frente de um
// engine real.

#include "modb/net/client.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/execution_context.hpp"
#include "modb/ops/operation_registry.hpp"
#include "modb/proxy/policies.hpp"
#include "modb/proxy/proxy.hpp"
#include "modb/proxy/token_policy.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace modb;
using namespace std::chrono_literals;

namespace {

std::string stamp() { return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()); }

void cleanup(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
}

net::Message call(std::string op) { return net::OpCall{.call_id = 1, .operation_id = std::move(op)}; }
net::Message query_of(std::uint64_t type) {
    return net::Query{.query_id = 1, .description = {.type = object::TypeDefinitionId{type}}};
}

ops::Caller user(std::string principal, std::vector<std::string> roles = {}) {
    return ops::Caller{.principal = std::move(principal), .roles = std::move(roles)};
}

ops::Caller anonymous(std::string address) {
    return ops::Caller{.attributes = {{"address", std::move(address)}}};
}

bool allowed(proxy::Policy& policy, const ops::Caller& caller, net::Message message) {
    return policy.authorize(caller, message).allowed;
}

struct Item {
    std::string name;
    std::int64_t value{};
};

object::BindingBuilder<Item> item_builder() {
    object::BindingBuilder<Item> builder{"Item"};
    builder.field<1>("name", &Item::name).field<2>("value", &Item::value);
    return builder;
}

template <typename F>
bool eventually_true(F condition, std::chrono::milliseconds limit = 5s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}

template <ops::OperationMode Mode>
class Named final : public ops::Operation {
public:
    static constexpr ops::OperationMode k_mode = Mode;
    static Result<std::unique_ptr<ops::Operation>> decode(std::span<const std::byte>) {
        return std::make_unique<Named>();
    }
    [[nodiscard]] std::string_view id() const noexcept override { return "t.named"; }
    Result<ops::OperationResult> execute(ops::ExecutionContext&) override { return ops::OperationResult{}; }
};

} // namespace

int main() {
    TestSuite suite;

    // --- read_only ---
    {
        proxy::ReadOnlyPolicy policy;
        suite.check(!allowed(policy, user("a"), call("app.ler")), "sem catálogo, nada passa");
        proxy::EngineInfo engine{.operations = {{"app.escrever", false}, {"app.ler", true}}};
        policy.on_engine(engine);
        suite.check(allowed(policy, user("a"), call("app.ler")), "proc de leitura passa");
        net::Message write = call("app.escrever");
        const auto decision = policy.authorize(user("a"), write);
        suite.check(!decision.allowed && decision.code == ErrorCode::permission_denied &&
                        decision.message.find("writes") != std::string::npos,
                    "proc de escrita é recusada");
        suite.check(!allowed(policy, user("a"), call("app.nada")), "proc desconhecida é recusada");
        suite.check(allowed(policy, user("a"), query_of(7)), "consulta passa");
        suite.check(engine.read_only("app.ler") == true && !engine.read_only("zzz"), "EngineInfo acha pelo id");
    }

    // --- allowlist ---
    {
        auto parsed = proxy::AllowlistPolicy::parse(
            "# quem  tipo  alvo\n"
            "leitor   call   app.ler*\n"
            "leitor   query  7\n"
            "user:ana *      *\n"
            "*        call   sys.procs   # todos\n"
            "admin    facade *\n");
        suite.check(parsed && (*parsed)->rules().size() == 5, "allowlist lida");
        if (parsed) {
            auto& policy = **parsed;
            const auto leitor = user("bruno", {"leitor"});
            suite.check(allowed(policy, leitor, call("app.ler_nota")), "prefixo com *");
            suite.check(!allowed(policy, leitor, call("app.apagar")), "fora da lista é recusado");
            suite.check(allowed(policy, leitor, query_of(7)) && !allowed(policy, leitor, query_of(8)),
                        "consulta pelo id do tipo");
            suite.check(allowed(policy, user("ana"), call("app.apagar")), "user:ana pode tudo");
            suite.check(!allowed(policy, user("anab"), call("app.apagar")), "user: é exato");
            suite.check(allowed(policy, anonymous("1.2.3.4:5"), call("sys.procs")), "* inclui anônimo");
            suite.check(!allowed(policy, anonymous("1.2.3.4:5"), call("app.ler")), "anônimo sem role");
            suite.check(allowed(policy, user("c", {"admin"}),
                                net::FacadeOpen{.request_id = 1, .facade_id = "Contas"}),
                        "facade por role");
            suite.check(!allowed(policy, leitor, net::FacadeList{.request_id = 1}), "FacadeList exige regra de facade");
        }
        suite.check_error(proxy::AllowlistPolicy::parse("leitor call\n"), ErrorCode::invalid_argument, "faltou o alvo");
        suite.check_error(proxy::AllowlistPolicy::parse("leitor apagar x\n"), ErrorCode::invalid_argument,
                          "tipo desconhecido");
    }

    // --- linha de auditoria ---
    {
        const auto ana = user("ana");
        const auto line = proxy::AuditLogPolicy::format(proxy::AuditRecord{.caller = &ana,
                                                                         .client = "10.0.0.1:5000",
                                                                         .kind = "call",
                                                                         .target = "app.ler",
                                                                         .ok = false,
                                                                         .code = ErrorCode::permission_denied,
                                                                         .denied = true,
                                                                         .duration = 1500us});
        suite.check(line == "audit call app.ler error " + std::to_string(static_cast<unsigned>(ErrorCode::permission_denied)) +
                                " 1.500ms denied by ana from 10.0.0.1:5000",
                    "formato da linha de auditoria: " + line);
        const auto query_line = proxy::AuditLogPolicy::format(
            proxy::AuditRecord{.kind = "query", .target = "7", .objects = 42});
        suite.check(query_line == "audit query 7 ok 0.000ms objects 42 by - from -", "consulta anônima: " + query_line);
    }

    // --- limites por principal ---
    {
        auto now = std::chrono::steady_clock::time_point{} + 1h;
        proxy::RateLimitPolicy policy{3, 2, [&] { return now; }};
        const auto ana = user("ana");
        int passed = 0;
        for (int i = 0; i < 5; ++i) {
            passed += allowed(policy, ana, call("x")) ? 1 : 0;
        }
        suite.check(passed == 3, "rajada de 3 chamadas");
        suite.check(allowed(policy, user("bia"), call("x")), "outro principal tem o próprio balde");
        now += 400ms;
        suite.check(allowed(policy, ana, call("x")) && !allowed(policy, ana, call("x")),
                    "400 ms devolvem uma chamada (3/s)");

        suite.check(allowed(policy, ana, query_of(1)) && allowed(policy, ana, query_of(1)) && !allowed(policy, ana, query_of(1)),
                    "no máximo 2 streams abertos");
        suite.check(policy.open_streams(ana) == 2, "2 streams contados");
        policy.audit(proxy::AuditRecord{.caller = &ana, .kind = "query", .ok = true});
        suite.check(policy.open_streams(ana) == 1 && allowed(policy, ana, query_of(1)), "o fim de um stream libera a vaga");
        policy.audit(proxy::AuditRecord{.caller = &ana, .kind = "query", .ok = false, .denied = true});
        suite.check(policy.open_streams(ana) == 2, "recusa não libera vaga");

        const auto a1 = anonymous("10.0.0.9:1000");
        const auto a2 = anonymous("10.0.0.9:2000");
        proxy::RateLimitPolicy hosts{1, 0, [&] { return now; }};
        suite.check(allowed(hosts, a1, call("x")) && !allowed(hosts, a2, call("x")),
                    "anônimos contam pela máquina, não pela porta");
    }

    // --- composição ---
    {
        auto tokens = proxy::TokenStore::parse(proxy::token_line("t1", "ana", std::vector<std::string>{"leitor"}));
        auto allowlist = proxy::AllowlistPolicy::parse("leitor call app.*\n");
        auto read_only = std::make_shared<proxy::ReadOnlyPolicy>();
        std::vector<std::string> lines;
        auto audit = std::make_shared<proxy::AuditLogPolicy>([&](std::string_view line) { lines.emplace_back(line); });
        proxy::PolicyChain chain{{std::make_shared<proxy::TokenPolicy>(std::move(*tokens), nullptr), *allowlist,
                                  read_only, nullptr, audit}};
        suite.check(chain.name() == "token+passthrough+allowlist+read_only+audit", "nome da cadeia: " +
                                                                                    std::string{chain.name()});
        suite.check(chain.mechanisms() == std::vector<std::string>{"token"}, "a cadeia anuncia o mecanismo do token");
        chain.on_engine(proxy::EngineInfo{.operations = {{"app.escrever", false}, {"app.ler", true}}});
        auto who = chain.authenticate({}, proxy::Credentials{.mechanism = "token",
                                                              .payload = {std::byte{'t'}, std::byte{'1'}}});
        suite.check(who && who->principal == "ana", "a cadeia autentica pelo token");
        if (who) {
            suite.check(allowed(chain, *who, call("app.ler")), "passa por allowlist e read_only");
            suite.check(!allowed(chain, *who, call("app.escrever")), "read_only recusa depois da allowlist");
            suite.check(!allowed(chain, *who, call("sys.procs")), "allowlist recusa");
            chain.audit(proxy::AuditRecord{.caller = &*who, .kind = "call", .target = "app.ler"});
            suite.check(lines.size() == 1 && lines[0].starts_with("audit call app.ler ok"), "auditoria no fim da cadeia");
        }
    }

    // --- de ponta a ponta ---
    const auto db = std::filesystem::temp_directory_path() / ("modb-x7-" + stamp() + ".modb");
    const auto sock = std::filesystem::current_path() / ("modb-x7-" + stamp() + ".sock");
    cleanup(db);
    auto server = net::Server::open(db);
    suite.check(server.has_value(), "engine aberto");
    if (!server) {
        return suite.finish();
    }
    suite.check(server->database().bind(item_builder()).has_value(), "Item ligado");
    const auto type_id = *server->database().type_id_of<Item>();
    {
        auto tx = server->database().begin();
        for (int i = 0; i < 3'000; ++i) {
            static_cast<void>(server->database().create(*tx, Item{std::string(40, 'n') + std::to_string(i), i}));
        }
        static_cast<void>(tx->commit());
    }
    auto registry = std::make_shared<ops::OperationRegistry>();
    static_cast<void>(registry->register_operation<Named<ops::OperationMode::read_only>>("app.ler"));
    static_cast<void>(registry->register_operation<Named<ops::OperationMode::read_write>>("app.escrever"));
    server->set_operation_registry(registry);
    suite.check(server->listen_local(sock).has_value(), "engine no link");
    std::thread engine{[&] { static_cast<void>(server->serve_forever()); }};
    const auto db_name = std::string{server->database_name()};

    auto tokens = proxy::TokenStore::parse(proxy::token_line("tok", "ana", std::vector<std::string>{"leitor"}));
    auto allowlist = proxy::AllowlistPolicy::parse("leitor call app.*\nleitor query " + std::to_string(type_id.value) + "\n");
    auto limits = std::make_shared<proxy::RateLimitPolicy>(0, 1);
    std::mutex lines_mu;
    std::vector<std::string> lines;
    auto audit = std::make_shared<proxy::AuditLogPolicy>([&](std::string_view line) {
        const std::scoped_lock lock{lines_mu};
        lines.emplace_back(line);
    });
    auto chain = std::make_shared<proxy::PolicyChain>(std::vector<std::shared_ptr<proxy::Policy>>{
        std::make_shared<proxy::TokenPolicy>(std::move(*tokens), nullptr), *allowlist,
        std::make_shared<proxy::ReadOnlyPolicy>(), limits, audit});
    auto px = proxy::Proxy::start(proxy::Options{.engine = sock, .port = 0, .stream_credit = 1}, chain);
    suite.check(px.has_value(), "proxy com a cadeia sobe");
    if (px) {
        std::thread runner{[&] { static_cast<void>(px->serve_forever()); }};
        {
            auto client = net::Client::connect("127.0.0.1", px->port(), db_name);
            suite.check(client && client->authenticate_token("tok").has_value(), "cliente autenticado");
            if (client) {
                suite.check(client->call("app.ler", {}).has_value(), "proc de leitura chega ao engine");
                auto write = client->call("app.escrever", {});
                suite.check(!write && write.error().code == ErrorCode::permission_denied,
                            "read_only recusa pelo catálogo que o engine mandou");
                auto first = client->query(net::QueryDescription{.type = type_id});
                auto second = client->query(net::QueryDescription{.type = type_id});
                auto refused = second ? second->next() : Result<std::optional<object::DecodedObject>>{};
                suite.check(!refused && refused.error().code == ErrorCode::permission_denied,
                            "segundo stream passa do limite por principal");
                std::size_t count = 0;
                while (first) {
                    auto next = first->next();
                    if (!next || !*next) {
                        break;
                    }
                    ++count;
                }
                suite.check(count == 3'000, "o primeiro stream termina");
                const auto ana = user("ana");
                suite.check(limits->open_streams(ana) == 0, "o fim do stream devolve a vaga");
            }
        }
        {
            // Um stream deixado aberto: um cliente que pede e nunca lê (o
            // `net::Client` leria tudo por baixo), e depois some.
            px->use_small_socket_buffers(true);
            auto raw = net::NativeSocket::connect("127.0.0.1", px->port());
            suite.check(raw.has_value(), "cliente cru conecta");
            if (raw) {
                static_cast<void>(raw->set_recv_buffer_bytes(4 * 1024));
                static_cast<void>(net::send_message(*raw, net::Hello{.database_name = db_name}));
                static_cast<void>(net::recv_message(*raw));
                static_cast<void>(net::send_message(
                    *raw, net::Authenticate{.request_id = 1, .mechanism = "token",
                                            .payload = {std::byte{'t'}, std::byte{'o'}, std::byte{'k'}}}));
                auto authenticated = net::recv_message(*raw);
                const auto* ok = authenticated ? std::get_if<net::AuthenticateOk>(&*authenticated) : nullptr;
                suite.check(ok != nullptr && ok->ok, "cliente cru autenticado");
                static_cast<void>(net::send_message(*raw, net::Query{.query_id = 9, .description = {.type = type_id}}));
                suite.check(eventually_true([&] { return limits->open_streams(user("ana")) == 1; }),
                            "stream parado ocupa a vaga");
                std::this_thread::sleep_for(100ms);
                suite.check(limits->open_streams(user("ana")) == 1, "e continua ocupando enquanto o cliente não lê");
                static_cast<void>(raw->close());
            }
        }
        suite.check(eventually_true([&] { return limits->open_streams(user("ana")) == 0; }),
                    "o cliente saiu: a auditoria fecha o stream e devolve a vaga");
        {
            const std::scoped_lock lock{lines_mu};
            const auto has = [&](std::string_view prefix, std::string_view part) {
                return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
                    return line.starts_with(prefix) && line.find(part) != std::string::npos;
                });
            };
            suite.check(has("audit authenticate token ok", "by ana"), "auditoria da autenticação");
            suite.check(has("audit call app.escrever error", "denied by ana"), "auditoria da recusa");
            suite.check(has("audit query " + std::to_string(type_id.value) + " ok", "objects 3000 by ana"),
                        "auditoria da consulta com o número de objetos");
            suite.check(has("audit query " + std::to_string(type_id.value) + " error", "by ana"),
                        "auditoria do stream abandonado");
        }
        px->request_stop();
        runner.join();
    }
    server->request_stop();
    engine.join();
    server = std::unexpected(Error{});
    cleanup(db);
    return suite.finish();
}
