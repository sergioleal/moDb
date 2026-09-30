// Protocolo minor 3 de ponta a ponta (ADR-029, PLANO_PROPOSTAS_REGISTRY R9–R11):
// servidor de aplicação (server::start) em --local e TCP, modb-proxy com tokens
// e auditoria, clientes de minor 3 (net::Client) e de minor 2 (frames à mão).
//
// - delegação: só com a role `delegate`; a proc vê principal e delegado; o
//   modo direto recusa; auditoria e log de chamadas mostram `as`;
// - detail: chega a quem negociou minor 3 e não a quem negociou minor 2;
// - idempotência: a mesma chave devolve o resultado sem executar de novo, por
//   principal, também depois de reabrir o servidor; resultado grande demais.
// - limites por delegado e teto por principal (RateLimitPolicy direto).

#include "modb/app/server_connection.hpp"
#include "modb/net/client.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/net/server.hpp"
#include "modb/proxy/policies.hpp"
#include "modb/proxy/proxy.hpp"
#include "modb/proxy/token_policy.hpp"
#include "modb/server/host.hpp"
#include "modb/server/module.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace modb;
using modb::ops::Value;
using modb::server::Context;
using modb::server::Mode;

namespace {

std::string stamp() { return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()); }

struct Evento {
    std::string texto;
};

object::BindingBuilder<Evento> evento_binding() {
    object::BindingBuilder<Evento> b{"Evento"};
    b.field<1>("texto", &Evento::texto);
    return b;
}

server::Module modulo_teste() {
    return server::ModuleBuilder{"t"}
        .type(evento_binding())
        .proc("t.quem", Mode::read_only, "quem chama",
              [](Context& c, const ops::Args&) -> Result<Value> {
                  const auto& who = c.caller();
                  return Value::object({{"principal", who.principal},
                                        {"subject", std::string{who.subject()}},
                                        {"delegated", who.delegated()},
                                        {"email", std::string{who.acting_attribute("email")}}});
              })
        .proc("t.contar", Mode::read_write, "grava um evento e devolve quantos há",
              [](Context& c, const ops::Args&) -> Result<Value> {
                  // `all` numa escrita vê o estado confirmado, sem o que a
                  // própria chamada grava: conta antes de criar.
                  auto antes = c.all<Evento>();
                  if (!antes) {
                      return std::unexpected(antes.error());
                  }
                  if (auto id = c.create(Evento{"e"}); !id) {
                      return std::unexpected(id.error());
                  }
                  return Value::object({{"n", static_cast<std::int64_t>(antes->size() + 1)}});
              })
        .proc("t.falhar", Mode::read_write, "erro com detail",
              [](Context& c, const ops::Args&) -> Result<Value> {
                  return c.fail(server::conflict("o endereço já está em uso"),
                                Value::object({{"reason", std::string{"handle_taken"}}, {"field", std::string{"handle"}}}));
              })
        .proc("t.grande", Mode::read_write, "grava e devolve um resultado maior que o registro de idempotência",
              [](Context& c, const ops::Args&) -> Result<Value> {
                  if (auto id = c.create(Evento{"g"}); !id) {
                      return std::unexpected(id.error());
                  }
                  return Value{std::string(6000, 'x')};
              })
        .build();
}

Result<Value> chamar(net::Client& client, std::string_view proc, const net::CallOptions& options = {}) {
    auto bytes = client.call(proc, ops::encode(Value::object({})), options);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return ops::decode(*bytes);
}

std::int64_t n_de(const Result<Value>& v) {
    if (!v) {
        std::cerr << "  (t.contar: erro " << static_cast<int>(v.error().code) << ": " << v.error().message << ")\n";
    }
    const auto* n = v ? v->field("n") : nullptr;
    return n != nullptr && n->integer() != nullptr ? *n->integer() : -1;
}

// Um cliente de minor 2: Hello minor 2, Authenticate, um OpCall, o OpResult cru.
Result<net::OpResult> chamada_minor2(std::uint16_t port, const std::string& token, std::string_view proc) {
    auto socket = net::NativeSocket::connect("127.0.0.1", port);
    if (!socket) {
        return std::unexpected(socket.error());
    }
    if (auto sent = net::send_message(*socket, net::Hello{.minor = 2}); !sent) {
        return std::unexpected(sent.error());
    }
    auto hello_ok = net::recv_message(*socket);
    const auto* ok = hello_ok ? std::get_if<net::HelloOk>(&*hello_ok) : nullptr;
    if (ok == nullptr || ok->minor != 2) {
        return std::unexpected(Error{ErrorCode::protocol_error, "HelloOk minor 2 esperado"});
    }
    if (!token.empty()) {
        std::vector<std::byte> payload(token.size());
        std::memcpy(payload.data(), token.data(), token.size());
        if (auto sent = net::send_message(*socket, net::Authenticate{.request_id = 1, .mechanism = "token", .payload = payload});
            !sent) {
            return std::unexpected(sent.error());
        }
        if (auto auth = net::recv_message(*socket); !auth) {
            return std::unexpected(auth.error());
        }
    }
    const auto args = ops::encode(Value::object({}));
    if (auto sent = net::send_message(*socket, net::OpCall{.call_id = 2, .operation_id = std::string{proc}, .args = args});
        !sent) {
        return std::unexpected(sent.error());
    }
    auto reply = net::recv_message(*socket);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (const auto* result = std::get_if<net::OpResult>(&*reply)) {
        return *result;
    }
    return std::unexpected(Error{ErrorCode::protocol_error, "OpResult esperado"});
}

std::string ler(const std::filesystem::path& file) {
    std::ifstream in{file};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace

int main() {
    TestSuite suite;

    // --- limites por delegado (sem rede) ---
    {
        const auto fixed = proxy::RateLimitPolicy::Clock::now();
        proxy::RateLimitPolicy limits{2, 0, [fixed] { return fixed; }, 3};
        net::Message call = net::OpCall{.operation_id = "x"};
        auto as = [](std::string who) { return ops::Caller{.principal = "gw", .acting_as = std::move(who)}; };
        const bool a1 = limits.authorize(as("u1"), call).allowed;
        const bool a2 = limits.authorize(as("u1"), call).allowed;
        const bool a3 = limits.authorize(as("u1"), call).allowed;
        suite.check(a1 && a2 && !a3, "o limite por segundo vale por delegado");
        suite.check(limits.authorize(as("u2"), call).allowed, "outro delegado tem o seu balde");
        const auto teto = limits.authorize(as("u3"), call);
        suite.check(!teto.allowed && teto.message.find("delegated calls") != std::string::npos,
                    "a soma dos delegados de um principal tem teto");
        suite.check(limits.authorize(ops::Caller{.principal = "outro"}, call).allowed, "sem delegação, o balde do principal");

        const ops::Caller delegado{.principal = "gw", .acting_as = "user:24"};
        const auto line = proxy::AuditLogPolicy::format(
            proxy::AuditRecord{.caller = &delegado, .client = "1.2.3.4:5", .kind = "call", .target = "x"});
        suite.check(line.find(" by gw as user:24 from 1.2.3.4:5") != std::string::npos, "a auditoria mostra 'by X as Y'");
    }

    // --- servidor de aplicação + proxy ---
    const auto pasta = std::filesystem::current_path() / ("modb-minor3-" + stamp());
    std::filesystem::create_directories(pasta);
    const auto db = pasta / "t.modb";
    const auto sock = pasta / "engine.sock";
    const auto log = pasta / "calls.log";
    const server::Module modulos[] = {modulo_teste()};
    const server::Options opcoes{.database = db, .host = "127.0.0.1", .port = 0, .local = sock, .tcp = true,
                                 .log = log.string()};

    auto tokens = proxy::TokenStore::parse(proxy::token_line("tok-gw", "gw", {"delegate"}) + "\n" +
                                           proxy::token_line("tok-app", "app", {"escritor"}) + "\n");
    std::mutex audit_mu;
    std::vector<std::string> audit_lines;
    auto audit = std::make_shared<proxy::AuditLogPolicy>([&](std::string_view l) {
        const std::scoped_lock lock{audit_mu};
        audit_lines.emplace_back(l);
    });
    auto chain = std::make_shared<proxy::PolicyChain>(std::vector<std::shared_ptr<proxy::Policy>>{
        std::make_shared<proxy::TokenPolicy>(std::move(*tokens), std::make_shared<proxy::PassThroughPolicy>()), audit});

    std::uint16_t engine_port = 0;
    {
        auto srv = server::start(opcoes, modulos);
        suite.check(srv.has_value(), "servidor de aplicação no link e em TCP");
        if (!srv) {
            return suite.finish();
        }
        engine_port = srv->port();
        std::thread laco{[&] { (void)srv->serve_forever(); }};
        auto px = proxy::Proxy::start(proxy::Options{.engine = sock, .port = 0}, chain);
        suite.check(px.has_value(), "proxy sobe");
        std::thread proxy_laco{[&] { (void)px->serve_forever(); }};
        const auto port = px ? px->port() : std::uint16_t{0};

        auto gw = net::Client::connect("127.0.0.1", port, "");
        auto app = net::Client::connect("127.0.0.1", port, "");
        suite.check(gw && app && gw->hello_ok().minor == 3, "proxy negocia minor 3");
        if (gw && app && gw->authenticate_token("tok-gw") && app->authenticate_token("tok-app")) {
            const net::Delegation ana{.subject = "user:24", .attributes = {{"email", "ana@exemplo.org"}}};

            // Delegação.
            auto quem = chamar(*gw, "t.quem", net::CallOptions{.acting_as = ana});
            suite.check(quem && *quem->field("principal")->text() == "gw" && *quem->field("subject")->text() == "user:24" &&
                            *quem->field("delegated")->boolean() && *quem->field("email")->text() == "ana@exemplo.org",
                        "a proc vê o principal, o delegado e os atributos dele");
            auto sem = chamar(*gw, "t.quem");
            suite.check(sem && *sem->field("subject")->text() == "gw" && !*sem->field("delegated")->boolean(),
                        "sem delegação, o subject é o principal");
            auto negado = chamar(*app, "t.quem", net::CallOptions{.acting_as = ana});
            suite.check(!negado && negado.error().code == ErrorCode::permission_denied,
                        "sem a role delegate, a delegação é recusada");

            // Detail.
            std::vector<std::byte> detail;
            auto falha = chamar(*gw, "t.falhar", net::CallOptions{.acting_as = ana, .error_detail = &detail});
            auto decoded = ops::decode(detail);
            suite.check(!falha && falha.error().code == ErrorCode::conflict && decoded &&
                            *decoded->field("reason")->text() == "handle_taken" &&
                            *decoded->field("field")->text() == "handle",
                        "o erro chega com o detail (reason, field) ao cliente de minor 3");
            auto velho = chamada_minor2(port, "tok-gw", "t.falhar");
            suite.check(velho && !velho->ok && velho->code == ErrorCode::conflict && velho->detail.empty(),
                        "um cliente de minor 2 recebe o erro sem o detail, pelo proxy");

            // Idempotência.
            const net::CallOptions k1{.idempotency_key = "k1"};
            const auto primeira = n_de(chamar(*gw, "t.contar", k1));
            const auto repetida = n_de(chamar(*gw, "t.contar", k1));
            const auto outra = n_de(chamar(*gw, "t.contar", net::CallOptions{.idempotency_key = "k2"}));
            suite.check(primeira == 1 && repetida == 1 && outra == 2,
                        "a mesma chave devolve o resultado sem executar de novo (1, 1, 2)");
            suite.check(n_de(chamar(*app, "t.contar", k1)) == 3, "a mesma chave de outro principal executa");
            suite.check(n_de(chamar(*gw, "t.contar")) == 4, "sem chave, executa sempre");
            auto leitura = chamar(*gw, "t.quem", net::CallOptions{.idempotency_key = "k-leitura"});
            suite.check(leitura.has_value(), "numa leitura a chave é ignorada");

            auto grande = chamar(*gw, "t.grande", net::CallOptions{.idempotency_key = "kg"});
            std::vector<std::byte> grande_detail;
            auto grande_de_novo = chamar(*gw, "t.grande", net::CallOptions{.idempotency_key = "kg", .error_detail = &grande_detail});
            auto gd = ops::decode(grande_detail);
            suite.check(grande && !grande_de_novo && grande_de_novo.error().code == ErrorCode::conflict && gd &&
                            *gd->field("reason")->text() == "idempotent_result_too_large",
                        "resultado grande demais: a repetição não executa, e diz por quê");
            suite.check(n_de(chamar(*gw, "t.contar")) == 6, "a repetição do resultado grande não gravou outro evento");
        }

        // Modo direto: sem proxy, não há quem autorize a delegação.
        auto direto = net::Client::connect("127.0.0.1", engine_port, "");
        if (direto) {
            auto d = chamar(*direto, "t.quem", net::CallOptions{.acting_as = net::Delegation{.subject = "x"}});
            suite.check(!d && d.error().code == ErrorCode::permission_denied, "o modo direto recusa a delegação");
            suite.check(n_de(chamar(*direto, "t.contar", net::CallOptions{.idempotency_key = "d1"})) == 7,
                        "chave no modo direto");
        }
        auto velho_direto = chamada_minor2(engine_port, "", "t.falhar");
        suite.check(velho_direto && !velho_direto->ok && velho_direto->detail.empty(),
                    "o engine direto não manda detail a um cliente de minor 2");

        if (px) {
            px->request_stop();
        }
        proxy_laco.join();
        srv->request_stop();
        laco.join();
    }

    {
        const std::scoped_lock lock{audit_mu};
        bool as = false;
        bool denied = false;
        for (const auto& l : audit_lines) {
            as = as || l.find("audit call t.quem ok") != std::string::npos && l.find(" by gw as user:24 ") != std::string::npos;
            denied = denied || (l.find("audit call t.quem error 74") != std::string::npos &&
                                l.find(" denied by app as user:24 ") != std::string::npos);
        }
        suite.check(as, "a auditoria do proxy registra 'by gw as user:24'");
        suite.check(denied, "a delegação recusada é auditada como denied, sem ir ao engine");
    }
    const auto calls = ler(log);
    suite.check(calls.find("call t.quem read") != std::string::npos && calls.find("by gw as user:24") != std::string::npos,
                "o log de chamadas do engine registra 'by gw as user:24'");
    suite.check(calls.find("reason handle_taken") != std::string::npos, "o log de chamadas grava o reason do detail");
    suite.check(calls.find("replayed") != std::string::npos, "o log de chamadas marca a chamada repetida pela chave");

    // A chave sobrevive à reabertura: ela foi gravada na transação da escrita.
    {
        auto srv = server::start(server::Options{.database = db, .host = "127.0.0.1", .port = 0, .log = "off"}, modulos);
        suite.check(srv.has_value(), "o servidor reabre o mesmo banco");
        if (srv) {
            std::thread laco{[&] { (void)srv->serve_forever(); }};
            {
                auto direto = net::Client::connect("127.0.0.1", srv->port(), "");
                suite.check(direto && n_de(chamar(*direto, "t.contar", net::CallOptions{.idempotency_key = "d1"})) == 7,
                            "depois de reabrir, a mesma chave devolve o resultado confirmado antes");
            }
            srv->request_stop();
            laco.join();
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(pasta, ignored);
    return suite.finish();
}
