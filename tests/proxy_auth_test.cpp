// Autenticação no proxy (ADR-028, PLANO_PROXY X6): SHA-256, arquivo de tokens,
// Authenticate/AuthenticateOk no codec (minor 2) e de ponta a ponta — pedido
// antes de autenticar, token errado, três erros fecham, token certo chega à
// proc com as roles, `ServerConnection` com token e o engine direto recusando
// Authenticate.

#include "modb/app/server_connection.hpp"
#include "modb/net/client.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/execution_context.hpp"
#include "modb/ops/operation_registry.hpp"
#include "modb/proxy/proxy.hpp"
#include "modb/proxy/token_policy.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
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

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// "principal|role,role|auth".
class WhoAmI final : public ops::Operation {
public:
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_only;
    static Result<std::unique_ptr<ops::Operation>> decode(std::span<const std::byte>) {
        return std::make_unique<WhoAmI>();
    }
    [[nodiscard]] std::string_view id() const noexcept override { return "t.whoami"; }
    Result<ops::OperationResult> execute(ops::ExecutionContext& context) override {
        const auto& caller = context.caller();
        std::string out = caller.principal + "|";
        for (std::size_t i = 0; i < caller.roles.size(); ++i) {
            out += (i == 0 ? "" : ",") + caller.roles[i];
        }
        out += "|" + std::string{caller.attribute("auth")};
        return ops::OperationResult{.payload = bytes_of(out)};
    }
};

template <typename M>
bool round_trips(const M& message) {
    auto bytes = net::encode_message(message);
    auto back = bytes ? net::decode_message(*bytes) : Result<net::Message>{std::unexpected(Error{})};
    const auto* typed = back ? std::get_if<M>(&*back) : nullptr;
    return typed != nullptr && *typed == message;
}

} // namespace

int main() {
    TestSuite suite;

    // --- SHA-256 (vetores do FIPS 180-4 e bordas de bloco) ---
    suite.check(proxy::to_hex(proxy::sha256("")) ==
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                "SHA-256 vazio");
    suite.check(proxy::to_hex(proxy::sha256("abc")) ==
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "SHA-256 abc");
    suite.check(proxy::to_hex(proxy::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                "SHA-256 de 448 bits (dois blocos no fim)");
    suite.check(proxy::to_hex(proxy::sha256(std::string(1'000'000, 'a'))) ==
                    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                "SHA-256 de um milhão de 'a'");
    suite.check(proxy::to_hex(proxy::sha256(std::string(64, 'x'))) != proxy::to_hex(proxy::sha256(std::string(63, 'x'))),
                "tamanhos vizinhos na borda do bloco dão resumos diferentes");

    // --- arquivo de tokens ---
    const std::vector<std::string> roles{"leitor", "admin"};
    const auto text = "# clientes\n" + proxy::token_line("tok-ana", "ana", roles) + "\n\n" +
                      proxy::token_line("tok-bia", "bia") + "  # sem roles\n";
    auto store = proxy::TokenStore::parse(text);
    suite.check(store && store->size() == 2, "tokens lidos, comentários e linhas vazias ignorados");
    if (store) {
        auto ana = store->find(bytes_of("tok-ana"));
        suite.check(ana && ana->principal == "ana" && ana->roles == roles, "token acha principal e roles");
        suite.check(!store->find(bytes_of("tok-an")), "token parecido não acha");
    }
    suite.check_error(proxy::TokenStore::parse("sha256:abc ana\n"), ErrorCode::invalid_argument, "hash curto");
    suite.check_error(proxy::TokenStore::parse("md5:" + std::string(64, 'a') + " ana\n"), ErrorCode::invalid_argument,
                      "prefixo errado");
    suite.check_error(proxy::TokenStore::parse(proxy::token_line("t", "p") + " x y\n"), ErrorCode::invalid_argument,
                      "palavras demais");

    // --- codec (minor 2) ---
    suite.check(net::protocol_minor == 2, "protocolo no minor 2");
    suite.check(round_trips(net::Authenticate{.request_id = 3, .mechanism = "token", .payload = bytes_of("x")}),
                "Authenticate ida e volta");
    suite.check(round_trips(net::AuthenticateOk{.request_id = 3, .principal = "ana"}), "AuthenticateOk ida e volta");
    suite.check(round_trips(net::AuthenticateOk{.request_id = 3, .ok = false, .message = "unknown token"}),
                "AuthenticateOk de recusa ida e volta");
    suite.check(round_trips(net::HelloOk{.auth_mechanisms = {"token", "mtls"}}), "HelloOk com mecanismos ida e volta");
    {
        // Um HelloOk minor 1 não carrega mecanismos, e um decoder que leia
        // minor 1 ignora os bytes a mais.
        auto bytes = net::encode_message(net::HelloOk{.minor = 1, .auth_mechanisms = {"token"}});
        auto back = bytes ? net::decode_message(*bytes) : Result<net::Message>{std::unexpected(Error{})};
        const auto* ok = back ? std::get_if<net::HelloOk>(&*back) : nullptr;
        suite.check(ok != nullptr && ok->auth_mechanisms.empty(), "minor 1 ignora os mecanismos");
    }

    // --- de ponta a ponta ---
    const auto db = std::filesystem::temp_directory_path() / ("modb-x6-" + stamp() + ".modb");
    const auto sock = std::filesystem::current_path() / ("modb-x6-" + stamp() + ".sock");
    cleanup(db);
    auto server = net::Server::open(db);
    suite.check(server.has_value(), "engine aberto");
    if (!server || !store) {
        return suite.finish();
    }
    auto registry = std::make_shared<ops::OperationRegistry>();
    static_cast<void>(registry->register_operation<WhoAmI>("t.whoami"));
    server->set_operation_registry(registry);
    suite.check(server->listen_local(sock) && server->listen_tcp("127.0.0.1", 0), "engine no link e em TCP");
    std::thread engine{[&] { static_cast<void>(server->serve_forever()); }};
    const auto db_name = std::string{server->database_name()};

    auto policy = std::make_shared<proxy::TokenPolicy>(std::move(*store), std::make_shared<proxy::PassThroughPolicy>());
    suite.check(policy->name() == "token+passthrough", "nome da política composta");
    auto px = proxy::Proxy::start(proxy::Options{.engine = sock, .port = 0}, policy);
    suite.check(px.has_value(), "proxy com tokens sobe");
    if (!px) {
        server->request_stop();
        engine.join();
        return suite.finish();
    }
    std::thread proxy_runner{[&] { static_cast<void>(px->serve_forever()); }};
    const auto port = px->port();

    {
        auto client = net::Client::connect("127.0.0.1", port, db_name);
        suite.check(client.has_value(), "cliente conecta");
        if (client) {
            suite.check(client->hello_ok().auth_mechanisms == std::vector<std::string>{"token"},
                        "HelloOk anuncia o mecanismo token");
            auto early = client->call("t.whoami", {});
            suite.check(!early && early.error().code == ErrorCode::unauthenticated,
                        "pedido antes de autenticar volta unauthenticated");
            auto early_query = client->query(net::QueryDescription{.type = object::TypeDefinitionId{1}});
            auto first = early_query ? early_query->next() : Result<std::optional<object::DecodedObject>>{};
            suite.check(!first && first.error().code == ErrorCode::unauthenticated,
                        "consulta antes de autenticar volta unauthenticated");
            suite.check(px->session_count() == 0, "sem sessão no engine antes de autenticar");

            auto wrong = client->authenticate_token("errado");
            suite.check(!wrong && wrong.error().code == ErrorCode::unauthenticated, "token errado é recusado");
            auto other = client->authenticate("senha", bytes_of("tok-ana"));
            suite.check(!other && other.error().code == ErrorCode::unauthenticated, "mecanismo desconhecido é recusado");

            auto principal = client->authenticate_token("tok-ana");
            suite.check(principal && *principal == "ana", "token certo devolve o principal");
            auto who = client->call("t.whoami", {});
            suite.check(who && text_of(*who) == "ana|leitor,admin|token", "a proc vê principal, roles e o mecanismo");
            auto again = client->authenticate_token("tok-bia");
            suite.check(!again && again.error().code == ErrorCode::invalid_argument, "segundo Authenticate é recusado");
            auto still = client->call("t.whoami", {});
            suite.check(still && text_of(*still).starts_with("ana|"), "a sessão continua a mesma");
        }
    }

    {
        auto client = net::Client::connect("127.0.0.1", port, db_name);
        if (client) {
            for (int i = 0; i < 3; ++i) {
                static_cast<void>(client->authenticate_token("errado"));
            }
            auto after = client->call("t.whoami", {});
            suite.check(!after && after.error().code != ErrorCode::unauthenticated,
                        "três tokens errados fecham a conexão");
        }
    }

    {
        auto connection = app::ServerConnection::connect(
            app::ConnectionOptions{.port = port, .database_name = db_name, .token = std::string{"tok-bia"}});
        suite.check(connection && connection->info().principal == "bia" &&
                        connection->info().auth_mechanisms == std::vector<std::string>{"token"},
                    "ServerConnection se autentica com o token das opções");
        if (connection) {
            auto who = connection->call("t.whoami", {});
            suite.check(who && text_of(*who) == "bia||token", "proc vê o cliente sem roles");
        }
        auto refused = app::ServerConnection::connect(
            app::ConnectionOptions{.port = port, .database_name = db_name, .token = std::string{"nada"}});
        suite.check(!refused && refused.error().code == ErrorCode::unauthenticated,
                    "ServerConnection com token errado falha no connect");
    }

    // --- o engine direto não autentica ---
    {
        auto direct = net::Client::connect("127.0.0.1", server->port(), db_name);
        auto auth = direct ? direct->authenticate_token("tok-ana") : Result<std::string>{std::unexpected(Error{})};
        suite.check(!auth && auth.error().code == ErrorCode::invalid_argument, "engine direto recusa Authenticate");
        auto who = direct ? direct->call("t.whoami", {}) : Result<std::vector<std::byte>>{std::unexpected(Error{})};
        suite.check(who && text_of(*who) == "||", "e segue atendendo como anônimo");
    }

    px->request_stop();
    proxy_runner.join();
    server->request_stop();
    engine.join();
    server = std::unexpected(Error{});
    cleanup(db);
    return suite.finish();
}
