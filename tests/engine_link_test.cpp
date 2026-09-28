// O engine atende o link dos proxies (ADR-028, PLANO_PROXY X4): abertura do
// link (versão e segredo), sessões multiplexadas com o principal visível às
// procs, chamadas de sessões diferentes em paralelo, crédito por stream (um
// stream sem crédito não para o outro), Cancel, fim de sessão no meio de um
// stream, queda do link, sessão desconhecida e TCP + link no mesmo servidor.

#include "modb/net/client.hpp"
#include "modb/net/link_protocol.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/execution_context.hpp"
#include "modb/ops/operation_registry.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace modb;
using namespace modb::net;
using namespace std::chrono_literals;

namespace {

std::string stamp() { return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()); }

std::filesystem::path temp_db_path(std::string_view tag) {
    return std::filesystem::temp_directory_path() / ("modb-x4-" + std::string{tag} + "-" + stamp() + ".modb");
}

// No diretório do teste: AF_UNIX sob %LOCALAPPDATA% falha em algumas máquinas Windows.
std::filesystem::path socket_path(std::string_view tag) {
    return std::filesystem::current_path() / ("modb-x4-" + std::string{tag} + "-" + stamp() + ".sock");
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

// Devolve o principal e as roles de quem chamou: "ana|leitor,admin".
class WhoAmI final : public ops::Operation {
public:
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_only;
    static Result<std::unique_ptr<ops::Operation>> decode(std::span<const std::byte>) {
        return std::make_unique<WhoAmI>();
    }
    [[nodiscard]] std::string_view id() const noexcept override { return "t.whoami"; }
    Result<ops::OperationResult> execute(ops::ExecutionContext& context) override {
        std::string out = context.caller().principal + "|";
        for (std::size_t i = 0; i < context.caller().roles.size(); ++i) {
            out += (i == 0 ? "" : ",") + context.caller().roles[i];
        }
        return ops::OperationResult{.payload = bytes_of(out)};
    }
};

// Dorme o número de milissegundos dado em texto nos argumentos.
class Nap final : public ops::Operation {
public:
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_only;
    static Result<std::unique_ptr<ops::Operation>> decode(std::span<const std::byte> args) {
        auto op = std::make_unique<Nap>();
        op->ms_ = std::stoi(std::string{reinterpret_cast<const char*>(args.data()), args.size()});
        return op;
    }
    [[nodiscard]] std::string_view id() const noexcept override { return "t.sleep"; }
    Result<ops::OperationResult> execute(ops::ExecutionContext&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms_));
        return ops::OperationResult{.payload = bytes_of("slept")};
    }

private:
    int ms_{0};
};

// Um proxy de teste: fala o link à mão e guarda os frames de cada sessão.
class TestLink {
public:
    static Result<TestLink> connect(const std::filesystem::path& path, std::string secret = {}) {
        auto socket = NativeSocket::connect_local(path);
        if (!socket) {
            return std::unexpected(socket.error());
        }
        // Um teste travado tem de falhar, não parar a suíte.
        static_cast<void>(socket->set_recv_timeout_ms(10'000));
        TestLink link{std::move(*socket)};
        if (auto sent = send_link_frame(link.socket_, LinkFrame{.session = 0,
                                                                .body = LinkControl{LinkHello{
                                                                    .proxy_name = "test", .secret = std::move(secret)}}});
            !sent) {
            return std::unexpected(sent.error());
        }
        auto reply = recv_link_frame(link.socket_);
        if (!reply) {
            return std::unexpected(reply.error());
        }
        const auto* control = std::get_if<LinkControl>(&reply->body);
        const auto* ok = control != nullptr ? std::get_if<LinkHelloOk>(control) : nullptr;
        if (ok == nullptr) {
            return std::unexpected(Error{ErrorCode::protocol_error, "expected LinkHelloOk"});
        }
        if (!ok->ok) {
            return std::unexpected(Error{ok->code, ok->message});
        }
        link.hello_ok_ = *ok;
        return link;
    }

    [[nodiscard]] const LinkHelloOk& hello_ok() const noexcept { return hello_ok_; }

    Result<void> open(std::uint32_t session, std::string principal, std::vector<std::string> roles = {}) {
        if (auto sent = send(session, LinkControl{SessionOpen{.principal = std::move(principal), .roles = std::move(roles)}});
            !sent) {
            return sent;
        }
        auto reply = next(session);
        if (!reply) {
            return std::unexpected(reply.error());
        }
        const auto* control = std::get_if<LinkControl>(&reply->body);
        const auto* ok = control != nullptr ? std::get_if<SessionOpenOk>(control) : nullptr;
        if (ok == nullptr || !ok->ok) {
            return std::unexpected(Error{ok ? ok->code : ErrorCode::protocol_error, ok ? ok->message : "no SessionOpenOk"});
        }
        return {};
    }

    Result<void> send(std::uint32_t session, Message message) {
        return send_link_frame(socket_, LinkFrame{.session = session, .body = std::move(message)});
    }
    Result<void> send(std::uint32_t session, LinkControl control) {
        return send_link_frame(socket_, LinkFrame{.session = session, .body = std::move(control)});
    }

    // Próximo frame da sessão (lê o link e guarda os das outras).
    Result<LinkFrame> next(std::uint32_t session) {
        auto& queue = pending_[session];
        while (queue.empty()) {
            auto frame = recv_link_frame(socket_);
            if (!frame) {
                return std::unexpected(frame.error());
            }
            pending_[frame->session].push_back(std::move(*frame));
        }
        auto frame = std::move(queue.front());
        queue.pop_front();
        return frame;
    }

    // Próxima mensagem do cliente na sessão (nullopt se veio controle).
    std::optional<Message> next_message(std::uint32_t session) {
        auto frame = next(session);
        if (!frame) {
            return std::nullopt;
        }
        if (auto* message = std::get_if<Message>(&frame->body)) {
            return std::move(*message);
        }
        return std::nullopt;
    }

    // Recebe o que já chegou da sessão sem esperar mais que `wait`.
    std::size_t buffered(std::uint32_t session) const {
        const auto found = pending_.find(session);
        return found == pending_.end() ? 0 : found->second.size();
    }

    NativeSocket& socket() noexcept { return socket_; }

private:
    explicit TestLink(NativeSocket socket) : socket_{std::move(socket)} {}

    NativeSocket socket_;
    LinkHelloOk hello_ok_{};
    std::map<std::uint32_t, std::deque<LinkFrame>> pending_;
};

// Lê a sessão até o fim do stream `query_id`; devolve (objetos, fim normal).
std::pair<std::size_t, bool> drain_stream(TestLink& link, std::uint32_t session, std::uint32_t query_id,
                                          std::uint32_t credit_per_frame) {
    std::size_t objects = 0;
    for (;;) {
        auto message = link.next_message(session);
        if (!message) {
            return {objects, false};
        }
        if (const auto* frame = std::get_if<ObjectFrame>(&*message)) {
            objects += frame->records.size();
            if (credit_per_frame > 0) {
                static_cast<void>(
                    link.send(session, LinkControl{StreamCredit{.query_id = query_id, .frames = credit_per_frame}}));
            }
            continue;
        }
        if (std::holds_alternative<StreamBegin>(*message)) {
            continue;
        }
        return {objects, std::holds_alternative<StreamEnd>(*message)};
    }
}

} // namespace

int main() {
    TestSuite suite;
    constexpr int total = 400;

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
            static_cast<void>(database->create(*tx, Item{"item-" + std::to_string(i), i}));
        }
        suite.check(tx->commit().has_value(), "itens gravados");
        object::DatabaseRegistry::instance().detach(*attached);
    }

    auto server = Server::open(db);
    suite.check(server.has_value(), "Server::open sem listener");
    if (!server) {
        return suite.finish();
    }
    suite.check(server->port() == 0 && server->local_path().empty(), "sem TCP e sem link antes de escutar");
    suite.check(server->database().bind(item_builder()).has_value(), "Item ligado no servidor");
    auto registry = std::make_shared<ops::OperationRegistry>();
    suite.check(registry->register_operation<WhoAmI>("t.whoami").has_value() &&
                    registry->register_operation<Nap>("t.sleep").has_value(),
                "operações de teste registradas");
    server->set_operation_registry(registry);
    server->set_max_concurrent_streams(2);
    server->set_link_secret("segredo");

    const auto sock = socket_path("engine");
    suite.check(server->listen_local(sock).has_value(), "listen_local");
    suite.check(server->listen_tcp("127.0.0.1", 0).has_value() && server->port() != 0, "listen_tcp junto");
    suite.check(!server->listen_local(sock), "segundo listen_local é recusado");

    Result<void> served{};
    std::thread runner{[&] { served = server->serve_forever(); }};
    std::this_thread::sleep_for(50ms);

    // --- abertura do link ---
    suite.check_error(TestLink::connect(sock, "errado"), ErrorCode::unauthenticated, "segredo errado é recusado");
    suite.check_error(TestLink::connect(sock), ErrorCode::unauthenticated, "sem segredo é recusado");
    {
        auto raw = NativeSocket::connect_local(sock);
        static_cast<void>(raw->set_recv_timeout_ms(5'000));
        static_cast<void>(send_link_frame(*raw, LinkFrame{.session = 0, .body = LinkControl{LinkHello{.version = 99}}}));
        auto reply = recv_link_frame(*raw);
        const auto* control = reply ? std::get_if<LinkControl>(&reply->body) : nullptr;
        const auto* ok = control ? std::get_if<LinkHelloOk>(control) : nullptr;
        suite.check(ok != nullptr && !ok->ok && ok->code == ErrorCode::incompatible_protocol_version,
                    "versão de link desconhecida é recusada");
    }

    auto link = TestLink::connect(sock, "segredo");
    suite.check(link.has_value(), "link aberto com o segredo");
    if (!link) {
        server->request_stop();
        runner.join();
        return suite.finish();
    }
    suite.check(link->hello_ok().database_name == server->database_name() &&
                    link->hello_ok().baseline == server->baseline() && link->hello_ok().max_concurrent_streams == 2,
                "LinkHelloOk traz nome, baseline e limite de streams");

    // --- sessões e principal ---
    suite.check(link->open(1, "ana", {"leitor", "admin"}).has_value(), "sessão 1 aberta");
    suite.check(link->open(2, "bruno").has_value(), "sessão 2 aberta");
    suite.check_error(link->open(1, "outro"), ErrorCode::invalid_argument, "sessão repetida é recusada");
    {
        static_cast<void>(link->send(1, OpCall{.call_id = 10, .operation_id = "t.whoami"}));
        static_cast<void>(link->send(2, OpCall{.call_id = 10, .operation_id = "t.whoami"}));
        auto a = link->next_message(1);
        auto b = link->next_message(2);
        const auto* ra = a ? std::get_if<OpResult>(&*a) : nullptr;
        const auto* rb = b ? std::get_if<OpResult>(&*b) : nullptr;
        suite.check(ra && ra->ok && ra->call_id == 10 && text_of(ra->payload) == "ana|leitor,admin",
                    "a proc vê o principal e as roles da sessão 1");
        suite.check(rb && rb->ok && text_of(rb->payload) == "bruno|", "a proc vê o principal da sessão 2");
    }

    // --- sessões diferentes em paralelo; na mesma sessão, em ordem ---
    {
        const auto start = std::chrono::steady_clock::now();
        static_cast<void>(link->send(1, OpCall{.call_id = 20, .operation_id = "t.sleep", .args = bytes_of("600")}));
        static_cast<void>(link->send(1, OpCall{.call_id = 21, .operation_id = "t.whoami"}));
        static_cast<void>(link->send(2, OpCall{.call_id = 22, .operation_id = "t.whoami"}));
        auto quick = link->next_message(2);
        const auto quick_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        suite.check(quick && std::holds_alternative<OpResult>(*quick) && quick_ms < 400ms,
                    "a sessão 2 responde sem esperar a proc lenta da sessão 1");
        auto first = link->next_message(1);
        auto second = link->next_message(1);
        const auto* r1 = first ? std::get_if<OpResult>(&*first) : nullptr;
        const auto* r2 = second ? std::get_if<OpResult>(&*second) : nullptr;
        suite.check(r1 && r2 && r1->call_id == 20 && r2->call_id == 21, "na sessão 1 as respostas saem em ordem");
    }

    // --- crédito: stream sem crédito não para o outro ---
    {
        static_cast<void>(link->send(1, Query{.query_id = 1, .description = {.type = type_id}}));
        static_cast<void>(link->send(1, LinkControl{StreamCredit{.query_id = 1, .frames = 1}}));
        auto begin = link->next_message(1);
        auto one = link->next_message(1);
        const auto* frame = one ? std::get_if<ObjectFrame>(&*one) : nullptr;
        suite.check(begin && std::holds_alternative<StreamBegin>(*begin) && frame != nullptr,
                    "com 1 crédito sai o StreamBegin e 1 frame");

        // A sessão 2 roda um stream inteiro enquanto a 1 espera crédito.
        static_cast<void>(link->send(2, Query{.query_id = 1, .description = {.type = type_id}}));
        static_cast<void>(link->send(2, LinkControl{StreamCredit{.query_id = 1, .frames = 4}}));
        const auto [objects2, ended2] = drain_stream(*link, 2, 1, 1);
        suite.check(ended2 && objects2 == total, "a sessão 2 termina o stream com a 1 parada");
        suite.check(link->buffered(1) == 0, "a sessão 1 não mandou nada sem crédito");

        static_cast<void>(link->send(1, LinkControl{StreamCredit{.query_id = 1, .frames = 1'000}}));
        const auto [objects1, ended1] = drain_stream(*link, 1, 1, 0);
        suite.check(ended1 && objects1 + frame->records.size() == total, "com crédito a sessão 1 termina");
    }

    // --- Cancel pelo link ---
    {
        static_cast<void>(link->send(1, Query{.query_id = 2, .description = {.type = type_id}}));
        static_cast<void>(link->send(1, LinkControl{StreamCredit{.query_id = 2, .frames = 1}}));
        static_cast<void>(link->next_message(1));  // StreamBegin
        static_cast<void>(link->next_message(1));  // 1 frame
        static_cast<void>(link->send(1, Cancel{.query_id = 2}));
        static_cast<void>(link->send(1, LinkControl{StreamCredit{.query_id = 2, .frames = 1'000}}));
        const auto [objects, ended] = drain_stream(*link, 1, 2, 0);
        suite.check(ended && objects < total, "Cancel encerra o stream com StreamEnd antes do fim");
    }

    // --- limite de streams por sessão ---
    {
        for (std::uint32_t q = 10; q < 13; ++q) {
            static_cast<void>(link->send(2, Query{.query_id = q, .description = {.type = type_id}}));
        }
        bool refused = false;
        for (int i = 0; i < 3 && !refused; ++i) {
            auto message = link->next_message(2);
            if (const auto* error = message ? std::get_if<StreamError>(&*message) : nullptr) {
                refused = error->query_id == 12 && error->message.find("max concurrent") != std::string::npos;
            }
        }
        suite.check(refused, "o terceiro stream da sessão passa do limite");
    }

    // --- fim de sessão no meio de um stream (sessão 2 tem 2 streams sem crédito) ---
    {
        static_cast<void>(link->send(2, LinkControl{SessionClose{}}));
        static_cast<void>(link->send(1, OpCall{.call_id = 30, .operation_id = "t.whoami"}));
        auto reply = link->next_message(1);
        suite.check(reply && std::holds_alternative<OpResult>(*reply), "outra sessão segue depois do SessionClose");
        static_cast<void>(link->send(2, OpCall{.call_id = 31, .operation_id = "t.whoami"}));
        bool closed_reply = false;
        for (int i = 0; i < 8 && !closed_reply; ++i) {
            auto frame = link->next(2);
            if (!frame) {
                break;
            }
            const auto* control = std::get_if<LinkControl>(&frame->body);
            closed_reply = control != nullptr && std::holds_alternative<SessionClose>(*control);
        }
        suite.check(closed_reply, "mensagem para sessão fechada volta como SessionClose");
    }

    // --- mensagem que não cabe numa sessão: o engine encerra a sessão ---
    {
        suite.check(link->open(3, "carla").has_value(), "sessão 3 aberta");
        static_cast<void>(link->send(3, Hello{}));
        auto frame = link->next(3);
        const auto* control = frame ? std::get_if<LinkControl>(&frame->body) : nullptr;
        const auto* close = control ? std::get_if<SessionClose>(control) : nullptr;
        suite.check(close && close->code == ErrorCode::protocol_error, "Hello numa sessão fecha a sessão");
    }

    // --- ShmAttach não é do engine atrás de proxy ---
    {
        static_cast<void>(link->send(1, ShmAttach{.request_id = 5}));
        auto reply = link->next_message(1);
        const auto* ok = reply ? std::get_if<ShmAttachOk>(&*reply) : nullptr;
        suite.check(ok && !ok->ok, "ShmAttach pelo link é recusado");
    }

    // --- queda do link no meio de um stream; o servidor segue ---
    {
        static_cast<void>(link->send(1, Query{.query_id = 3, .description = {.type = type_id}}));
        static_cast<void>(link->next_message(1));  // StreamBegin, e fica sem crédito
        static_cast<void>(link->socket().close());
        auto again = TestLink::connect(sock, "segredo");
        suite.check(again.has_value() && again->open(1, "ana").has_value(), "novo link depois da queda");
        if (again) {
            static_cast<void>(again->send(1, OpCall{.call_id = 1, .operation_id = "t.whoami"}));
            auto reply = again->next_message(1);
            suite.check(reply && std::holds_alternative<OpResult>(*reply), "novo link atende");
        }
    }

    // --- o TCP direto segue funcionando junto ---
    {
        auto client = Client::connect("127.0.0.1", server->port(), std::string{server->database_name()});
        suite.check(client.has_value(), "cliente TCP direto conecta");
        if (client) {
            auto result = client->call("t.whoami", {});
            suite.check(result && text_of(*result) == "|", "no TCP direto o chamador é anônimo");
        }
    }

    server->request_stop();
    runner.join();
    suite.check(served.has_value(), "serve_forever para limpo com TCP e link");
    const auto socket_file = server->local_path();
    server = std::unexpected(Error{});
    suite.check(!std::filesystem::exists(socket_file), "o socket sai com o servidor");
    cleanup(db);
    return suite.finish();
}
