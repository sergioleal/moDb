// Proxy de acesso remoto de ponta a ponta (ADR-028, PLANO_PROXY X5): o
// cliente de sempre (`net::Client`) fala com o proxy, o proxy com o engine por
// um link local. Handshake, streams com RLE, várias streams num cliente,
// Cancel, OpCall com o chamador visto pela proc, política que recusa e
// reescreve, auditoria, um cliente parado que não para os outros (crédito),
// queda e volta do engine, ShmAttach recusado e parada limpa.

#include "modb/net/client.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/execution_context.hpp"
#include "modb/ops/operation_registry.hpp"
#include "modb/proxy/proxy.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace modb;
using namespace std::chrono_literals;

namespace {

std::string stamp() { return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()); }

std::filesystem::path temp_db_path(std::string_view tag) {
    return std::filesystem::temp_directory_path() / ("modb-x5-" + std::string{tag} + "-" + stamp() + ".modb");
}

// No diretório do teste: AF_UNIX sob %LOCALAPPDATA% falha em algumas máquinas Windows.
std::filesystem::path socket_path(std::string_view tag) {
    return std::filesystem::current_path() / ("modb-x5-" + std::string{tag} + "-" + stamp() + ".sock");
}

void cleanup(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
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

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// "principal|address" de quem chamou.
class WhoAmI final : public ops::Operation {
public:
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_only;
    static Result<std::unique_ptr<ops::Operation>> decode(std::span<const std::byte>) {
        return std::make_unique<WhoAmI>();
    }
    [[nodiscard]] std::string_view id() const noexcept override { return "t.whoami"; }
    Result<ops::OperationResult> execute(ops::ExecutionContext& context) override {
        return ops::OperationResult{
            .payload = bytes_of(context.caller().principal + "|" + std::string{context.caller().attribute("address")})};
    }
};

// Devolve os argumentos.
class Echo final : public ops::Operation {
public:
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_only;
    static Result<std::unique_ptr<ops::Operation>> decode(std::span<const std::byte> args) {
        auto op = std::make_unique<Echo>();
        op->args_.assign(args.begin(), args.end());
        return op;
    }
    [[nodiscard]] std::string_view id() const noexcept override { return "t.echo"; }
    Result<ops::OperationResult> execute(ops::ExecutionContext&) override {
        return ops::OperationResult{.payload = args_};
    }

private:
    std::vector<std::byte> args_;
};

// Política de teste: dá nome ao cliente, recusa `t.secret`, reescreve os
// argumentos do `t.echo`, marca as respostas do `t.whoami` e guarda a auditoria.
class TestPolicy final : public proxy::Policy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "test"; }

    Result<ops::Caller> authenticate(const proxy::ClientInfo& client, const proxy::Credentials& credentials) override {
        auto caller = Policy::authenticate(client, credentials);
        if (caller) {
            caller->principal = "cliente";
        }
        return caller;
    }

    proxy::Decision authorize(const ops::Caller&, net::Message& request) override {
        if (auto* call = std::get_if<net::OpCall>(&request)) {
            if (call->operation_id == "t.secret") {
                return proxy::Decision::deny("t.secret is not for you");
            }
            if (call->operation_id == "t.echo") {
                call->args = bytes_of("reescrito");
            }
        }
        return proxy::Decision::allow();
    }

    void on_response(const ops::Caller&, net::Message& response) override {
        if (auto* result = std::get_if<net::OpResult>(&response); result && text_of(result->payload).starts_with("cliente|")) {
            auto text = text_of(result->payload) + "|visto";
            result->payload = bytes_of(text);
        }
    }

    void audit(const proxy::AuditRecord& record) override {
        const std::scoped_lock lock{mu_};
        records_.push_back(std::string{record.kind} + " " + std::string{record.target} + (record.ok ? " ok" : " fail") +
                           (record.denied ? " denied" : "") + " " + std::to_string(record.objects) + " " +
                           record.caller->principal);
    }

    std::vector<std::string> records() const {
        const std::scoped_lock lock{mu_};
        return records_;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::string> records_;
};

// O engine do teste: banco com `total` itens, procs de teste, só o link local.
struct Engine {
    std::optional<net::Server> server;
    std::thread runner;
    Result<void> served{};

    Result<void> start(const std::filesystem::path& db, const std::filesystem::path& sock) {
        auto opened = net::Server::open(db);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        server.emplace(std::move(*opened));
        if (auto bound = server->database().bind(item_builder()); !bound) {
            return bound;
        }
        auto registry = std::make_shared<ops::OperationRegistry>();
        static_cast<void>(registry->register_operation<WhoAmI>("t.whoami"));
        static_cast<void>(registry->register_operation<Echo>("t.echo"));
        static_cast<void>(registry->register_operation<Echo>("t.secret"));
        server->set_operation_registry(registry);
        server->set_link_secret("s3");
        if (auto listening = server->listen_local(sock); !listening) {
            return listening;
        }
        runner = std::thread{[this] { served = server->serve_forever(); }};
        return {};
    }

    void stop() {
        if (server) {
            server->request_stop();
        }
        if (runner.joinable()) {
            runner.join();
        }
        server.reset();
    }

    ~Engine() { stop(); }
};

// Espera `condition` por até `limit`.
template <typename F>
bool eventually(F condition, std::chrono::milliseconds limit = 5s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}

std::size_t drain(net::ObjectStream& stream, std::int64_t* sum = nullptr) {
    std::size_t count = 0;
    for (;;) {
        auto next = stream.next();
        if (!next || !*next) {
            return count;
        }
        ++count;
        if (sum != nullptr) {
            for (const auto& [field, attr] : (*next)->fields) {
                if (auto value = field.value == 2 ? attr.as_int64() : Result<std::int64_t>{std::unexpected(Error{})}) {
                    *sum += *value;
                }
            }
        }
    }
}

} // namespace

int main() {
    TestSuite suite;
    constexpr int total = 3'000;

    const auto db = temp_db_path("db");
    cleanup(db);
    object::TypeDefinitionId type_id{};
    {
        auto created = object::Database::create(db);
        suite.check(created.has_value(), "banco criado");
        if (!created) {
            return suite.finish();
        }
        auto database = std::make_shared<object::Database>(std::move(*created));
        auto attached = object::DatabaseRegistry::instance().attach(database);
        suite.check(database->bind(item_builder()).has_value(), "Item ligado");
        type_id = *database->type_id_of<Item>();
        auto tx = database->begin();
        for (int i = 0; i < total; ++i) {
            // Nomes repetitivos: o RLE tem o que comprimir.
            static_cast<void>(database->create(*tx, Item{std::string(40, 'a') + std::to_string(i % 7), i}));
        }
        suite.check(tx->commit().has_value(), "itens gravados");
        object::DatabaseRegistry::instance().detach(*attached);
    }

    const auto sock = socket_path("engine");
    Engine engine;
    suite.check(engine.start(db, sock).has_value(), "engine escuta só no link");
    if (!engine.server) {
        return suite.finish();
    }
    const auto baseline = engine.server->baseline();
    const auto db_name = std::string{engine.server->database_name()};

    suite.check_error(proxy::Proxy::start(proxy::Options{.engine = sock, .link_secret = "errado", .port = 0},
                                          std::make_shared<proxy::PassThroughPolicy>()),
                      ErrorCode::unauthenticated, "proxy com segredo errado não sobe");
    suite.check_error(proxy::Proxy::start(proxy::Options{.engine = socket_path("ninguem"), .port = 0},
                                          std::make_shared<proxy::PassThroughPolicy>()),
                      ErrorCode::io_error, "proxy sem engine não sobe");

    auto policy = std::make_shared<TestPolicy>();
    auto started = proxy::Proxy::start(proxy::Options{.engine = sock,
                                                      .link_secret = "s3",
                                                      .name = "teste",
                                                      .port = 0,
                                                      .stream_credit = 2,
                                                      .reconnect_max_ms = 200},
                                       policy);
    suite.check(started.has_value(), "proxy sobe");
    if (!started) {
        return suite.finish();
    }
    auto& px = *started;
    px.use_small_socket_buffers(true);
    Result<void> proxy_served{};
    std::thread proxy_runner{[&] { proxy_served = px.serve_forever(); }};
    const auto port = px.port();
    suite.check(px.link_up(), "link aberto");

    // --- handshake, stream com RLE, várias streams, Cancel ---
    {
        auto client = net::Client::connect("127.0.0.1", port, db_name);
        suite.check(client.has_value(), "cliente conecta no proxy");
        if (client) {
            suite.check(client->hello_ok().baseline == baseline, "HelloOk traz a baseline do engine");
            suite.check(client->hello_ok().selected_codec == net::Compression::rle, "proxy negocia RLE com o cliente");
            suite.check(eventually([&] { return px.session_count() == 1; }), "sessão aberta no engine");

            auto stream = client->query(net::QueryDescription{.type = type_id});
            std::int64_t sum = 0;
            const auto count = stream ? drain(*stream, &sum) : 0;
            suite.check(count == total && sum == std::int64_t{total} * (total - 1) / 2,
                        "stream inteiro pelo proxy, valores certos");

            auto a = client->query(net::QueryDescription{.type = type_id});
            auto b = client->query(net::QueryDescription{.type = type_id, .limit = 100});
            suite.check(a && b, "duas streams no mesmo cliente");
            if (a && b) {
                suite.check(drain(*b) == 100 && drain(*a) == total, "as duas terminam, cada uma com o seu");
            }

            auto c = client->query(net::QueryDescription{.type = type_id});
            if (c) {
                auto first = c->next();
                suite.check(first && *first, "primeiro objeto antes do Cancel");
                suite.check(client->cancel(c->query_id()).has_value(), "Cancel enviado");
                suite.check(drain(*c) + 1 < total, "Cancel encerra a stream antes do fim");
            }
        }
    }
    suite.check(eventually([&] { return px.session_count() == 0; }), "cliente saiu, sessão fechada");

    // --- OpCall, chamador, política ---
    {
        auto client = net::Client::connect("127.0.0.1", port, db_name);
        if (client) {
            auto who = client->call("t.whoami", {});
            suite.check(who && text_of(*who).starts_with("cliente|127.0.0.1:") && text_of(*who).ends_with("|visto"),
                        "a proc vê o principal e o endereço dados pelo proxy; a resposta passa pela política");
            auto echoed = client->call("t.echo", bytes_of("original"));
            suite.check(echoed && text_of(*echoed) == "reescrito", "a política reescreve os argumentos");
            auto secret = client->call("t.secret", {});
            suite.check(!secret && secret.error().code == ErrorCode::permission_denied,
                        "a política recusa com permission_denied");
            auto missing = client->call("t.nada", {});
            suite.check(!missing && missing.error().code == ErrorCode::operation_not_found,
                        "erro do engine chega ao cliente");
            auto facades = client->list_facades();
            suite.check(facades && facades->empty(), "FacadeList passa pelo proxy");
            auto shm = client->attach_shared_memory();
            suite.check(!shm, "ShmAttach não é atendido por este proxy");
            auto after = client->call("t.echo", {});
            suite.check(after.has_value(), "a sessão segue depois das recusas");
        }
    }
    {
        const auto records = policy->records();
        const auto has = [&](std::string_view line) {
            return std::find(records.begin(), records.end(), line) != records.end();
        };
        suite.check(has("call t.whoami ok 0 cliente") && has("call t.secret fail denied 0 cliente") &&
                        has("call t.nada fail 0 cliente") && has("query " + std::to_string(type_id.value) + " ok " +
                                                                 std::to_string(total) + " cliente"),
                    "auditoria de chamadas, recusas e consultas");
    }

    // --- um cliente parado não para os outros (crédito por stream) ---
    {
        auto stuck = net::NativeSocket::connect("127.0.0.1", port);
        suite.check(stuck.has_value(), "cliente parado conecta");
        if (stuck) {
            static_cast<void>(stuck->set_recv_buffer_bytes(4 * 1024));
            static_cast<void>(net::send_message(*stuck, net::Hello{.database_name = db_name}));
            auto hello_ok = net::recv_message(*stuck);
            suite.check(hello_ok && std::holds_alternative<net::HelloOk>(*hello_ok), "HelloOk do cliente parado");
            // Pede o banco inteiro e nunca lê.
            static_cast<void>(net::send_message(*stuck, net::Query{.query_id = 1, .description = {.type = type_id}}));
            std::this_thread::sleep_for(200ms);

            const auto start = std::chrono::steady_clock::now();
            auto other = net::Client::connect("127.0.0.1", port, db_name);
            auto stream = other ? other->query(net::QueryDescription{.type = type_id})
                               : Result<net::ObjectStream>{std::unexpected(Error{})};
            const auto count = stream ? drain(*stream) : 0;
            suite.check(count == total && std::chrono::steady_clock::now() - start < 10s,
                        "outro cliente lê tudo com o parado segurando o dele");
            const auto stats = engine.server->last_stream_stats();
            suite.check(stats.max_outstanding <= net::max_in_flight_objects, "engine com fila limitada");
            static_cast<void>(stuck->close());
        }
    }

    // --- engine cai e volta: clientes saem, o proxy reabre o link ---
    {
        auto client = net::Client::connect("127.0.0.1", port, db_name);
        suite.check(client.has_value(), "cliente antes da queda");
        engine.stop();
        suite.check(eventually([&] { return !px.link_up(); }), "proxy percebe a queda do engine");
        if (client) {
            auto failed = client->call("t.echo", {});
            suite.check(!failed, "chamada de um cliente da sessão perdida falha");
        }
        auto refused = net::Client::connect("127.0.0.1", port, db_name);
        suite.check(!refused, "sem engine, o proxy não aceita clientes");

        suite.check(engine.start(db, sock).has_value(), "engine volta");
        suite.check(eventually([&] { return px.link_up(); }), "proxy reabre o link");
        auto again = net::Client::connect("127.0.0.1", port, db_name);
        auto echoed = again ? again->call("t.echo", {}) : Result<std::vector<std::byte>>{std::unexpected(Error{})};
        suite.check(echoed.has_value(), "cliente novo funciona depois da volta");
    }

    px.request_stop();
    proxy_runner.join();
    suite.check(proxy_served.has_value(), "serve_forever do proxy para limpo");
    engine.stop();
    cleanup(db);
    return suite.finish();
}
