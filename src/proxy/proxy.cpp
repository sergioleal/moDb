#include "modb/proxy/proxy.hpp"

#include "modb/net/link_protocol.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/net/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace modb::proxy {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t k_small_socket_buffer = 4 * 1024;
// Espera pela resposta do engine ao SessionOpen.
constexpr auto k_open_timeout = std::chrono::seconds{10};
constexpr auto k_first_reconnect = std::chrono::milliseconds{50};

Error closed(std::string message) { return Error{ErrorCode::connection_closed, std::move(message)}; }

// Um pedido a caminho do engine, para a auditoria quando a resposta voltar.
struct Pending {
    std::string_view kind;
    std::string target;
    Clock::time_point start{};
    std::uint64_t objects{0};
};

// O que só o cliente manda numa sessão (o resto é resposta ou abertura).
[[nodiscard]] bool is_client_request(const net::Message& message) noexcept {
    return std::holds_alternative<net::Query>(message) || std::holds_alternative<net::Cancel>(message) ||
           std::holds_alternative<net::OpCall>(message) || std::holds_alternative<net::FacadeList>(message) ||
           std::holds_alternative<net::FacadeOpen>(message) || std::holds_alternative<net::ShmAttach>(message);
}

// Resposta que o proxy dá sozinho a um pedido que a política recusou.
[[nodiscard]] std::optional<net::Message> denial(const net::Message& request, const Decision& decision) {
    if (const auto* call = std::get_if<net::OpCall>(&request)) {
        return net::OpResult{.call_id = call->call_id, .ok = false, .code = decision.code, .message = decision.message};
    }
    if (const auto* query = std::get_if<net::Query>(&request)) {
        return net::StreamError{.query_id = query->query_id, .code = decision.code, .message = decision.message};
    }
    if (const auto* list = std::get_if<net::FacadeList>(&request)) {
        // A lista recusada volta vazia: o protocolo não tem erro no FacadeListOk.
        return net::FacadeListOk{.request_id = list->request_id};
    }
    if (const auto* open = std::get_if<net::FacadeOpen>(&request)) {
        return net::FacadeOpenOk{.request_id = open->request_id,
                                 .ok = false,
                                 .code = decision.code,
                                 .message = decision.message,
                                 .facade_id = open->facade_id,
                                 .facade_version = open->facade_version};
    }
    return std::nullopt;
}

// Tipo, id e alvo de um pedido para a auditoria.
struct RequestKey {
    std::string_view kind;
    std::uint32_t id{0};
    std::string target;
};

[[nodiscard]] std::optional<RequestKey> request_key(const net::Message& request) {
    if (const auto* call = std::get_if<net::OpCall>(&request)) {
        return RequestKey{"call", call->call_id, call->operation_id};
    }
    if (const auto* query = std::get_if<net::Query>(&request)) {
        return RequestKey{"query", query->query_id, "type " + std::to_string(query->description.type.value)};
    }
    if (const auto* list = std::get_if<net::FacadeList>(&request)) {
        return RequestKey{"facade_list", list->request_id, {}};
    }
    if (const auto* open = std::get_if<net::FacadeOpen>(&request)) {
        return RequestKey{"facade_open", open->request_id,
                          open->facade_id + " v" + std::to_string(open->facade_version)};
    }
    return std::nullopt;
}

} // namespace

// Um cliente com sessão aberta no engine.
struct ClientSession {
    std::uint32_t id{0};
    std::shared_ptr<net::NativeSocket> socket;
    net::Compression codec{net::Compression::none};
    ops::Caller caller;
    std::string address;

    // Saída para o cliente: a thread do link empilha, a escritora do cliente
    // manda. Assim um cliente lento nunca segura a thread do link.
    std::mutex out_mu;
    std::condition_variable out_cv;
    std::deque<net::Message> out;
    bool out_closed{false};

    std::mutex open_mu;
    std::condition_variable open_cv;
    std::optional<net::SessionOpenOk> open_reply;

    std::atomic<bool> engine_closed{false};

    // Pedidos a caminho, por tipo e id (ids são escopados pelo cliente).
    std::mutex pending_mu;
    std::unordered_map<std::uint32_t, Pending> calls;
    std::unordered_map<std::uint32_t, Pending> queries;
    std::unordered_map<std::uint32_t, Pending> facades;

    void push(net::Message message) {
        {
            const std::scoped_lock lock{out_mu};
            if (out_closed) {
                return;
            }
            out.push_back(std::move(message));
        }
        out_cv.notify_one();
    }

    void close_out() {
        {
            const std::scoped_lock lock{out_mu};
            out_closed = true;
        }
        out_cv.notify_all();
    }

    void opened(net::SessionOpenOk reply) {
        {
            const std::scoped_lock lock{open_mu};
            if (!open_reply) {
                open_reply = std::move(reply);
            }
        }
        open_cv.notify_all();
    }

    // O engine encerrou a sessão (ou o link caiu): o cliente sai.
    void engine_close() {
        engine_closed.store(true);
        opened(net::SessionOpenOk{.ok = false, .code = ErrorCode::connection_closed, .message = "engine closed"});
        static_cast<void>(socket->shutdown());
    }

    std::unordered_map<std::uint32_t, Pending>& pending_for(std::string_view kind) {
        return kind == "call" ? calls : kind == "query" ? queries : facades;
    }
};

struct Proxy::Impl {
    Options options;
    std::shared_ptr<Policy> policy;
    net::NativeSocket listener;
    std::uint16_t port{0};
    std::atomic<bool> small_buffers{false};
    std::atomic<bool> stop{false};

    // --- link ---
    // `link_mu` serializa a escrita no link e a troca do socket na reconexão.
    std::mutex link_mu;
    std::shared_ptr<net::NativeSocket> link;
    net::LinkHelloOk engine{};
    std::atomic<bool> link_is_up{false};
    std::thread link_thread;
    std::mutex backoff_mu;
    std::condition_variable backoff_cv;

    // --- sessões e clientes ---
    mutable std::mutex sessions_mu;
    std::unordered_map<std::uint32_t, std::shared_ptr<ClientSession>> sessions;
    std::uint32_t next_session{1};

    std::mutex clients_mu;
    std::uint64_t next_client{0};
    std::unordered_map<std::uint64_t, std::shared_ptr<net::NativeSocket>> clients;
    // Clientes que já saíram: o laço de accept junta as threads deles.
    std::vector<std::uint64_t> finished;

    [[nodiscard]] Result<std::pair<std::shared_ptr<net::NativeSocket>, net::LinkHelloOk>> open_link() const {
        auto socket = net::NativeSocket::connect_local(options.engine);
        if (!socket) {
            return std::unexpected(socket.error());
        }
        auto shared = std::make_shared<net::NativeSocket>(std::move(*socket));
        const net::LinkFrame hello{
            .session = net::link_control_session,
            .body = net::LinkControl{net::LinkHello{.proxy_name = options.name, .secret = options.link_secret}},
        };
        if (auto sent = net::send_link_frame(*shared, hello); !sent) {
            return std::unexpected(sent.error());
        }
        auto reply = net::recv_link_frame(*shared);
        if (!reply) {
            return std::unexpected(reply.error());
        }
        const auto* control = std::get_if<net::LinkControl>(&reply->body);
        const auto* ok = control != nullptr ? std::get_if<net::LinkHelloOk>(control) : nullptr;
        if (ok == nullptr) {
            return std::unexpected(Error{ErrorCode::protocol_error, "engine did not answer LinkHello"});
        }
        if (!ok->ok) {
            return std::unexpected(Error{ok->code, "engine refused the link: " + ok->message});
        }
        return std::pair{std::move(shared), *ok};
    }

    void set_link(std::shared_ptr<net::NativeSocket> socket, const net::LinkHelloOk& hello) {
        const std::scoped_lock lock{link_mu};
        link = std::move(socket);
        engine = hello;
        link_is_up.store(true);
    }

    [[nodiscard]] Result<void> send_engine(std::uint32_t session, const net::Message& message) {
        const std::scoped_lock lock{link_mu};
        if (!link) {
            return std::unexpected(closed("link to the engine is down"));
        }
        return net::send_link_message(*link, session, message);
    }

    [[nodiscard]] Result<void> send_engine(std::uint32_t session, net::LinkControl control) {
        const std::scoped_lock lock{link_mu};
        if (!link) {
            return std::unexpected(closed("link to the engine is down"));
        }
        return net::send_link_frame(*link, net::LinkFrame{.session = session, .body = std::move(control)});
    }

    [[nodiscard]] std::shared_ptr<ClientSession> find(std::uint32_t id) const {
        const std::scoped_lock lock{sessions_mu};
        const auto found = sessions.find(id);
        return found == sessions.end() ? nullptr : found->second;
    }

    // Um frame do engine. false = o link está quebrado (erro de protocolo).
    [[nodiscard]] bool dispatch(net::LinkFrame& frame) {
        if (frame.session == net::link_control_session) {
            return false;
        }
        auto session = find(frame.session);
        if (!session) {
            return true;  // o cliente já saiu; resposta a caminho
        }
        if (auto* message = std::get_if<net::Message>(&frame.body)) {
            session->push(std::move(*message));
            return true;
        }
        auto& control = std::get<net::LinkControl>(frame.body);
        if (auto* reply = std::get_if<net::SessionOpenOk>(&control)) {
            session->opened(std::move(*reply));
            return true;
        }
        if (std::holds_alternative<net::SessionClose>(control)) {
            session->engine_close();
            return true;
        }
        return false;
    }

    void drop_link(const std::shared_ptr<net::NativeSocket>& socket) {
        {
            const std::scoped_lock lock{link_mu};
            if (link == socket) {
                link.reset();
            }
            link_is_up.store(false);
        }
        static_cast<void>(socket->shutdown());
        static_cast<void>(socket->close());
        std::vector<std::shared_ptr<ClientSession>> all;
        {
            const std::scoped_lock lock{sessions_mu};
            for (auto& [id, session] : sessions) {
                (void)id;
                all.push_back(session);
            }
        }
        for (const auto& session : all) {
            session->engine_close();
        }
    }

    // Lê o link e despacha; quando ele cai, fecha os clientes e tenta reabrir
    // com espera crescente até conseguir ou o proxy parar.
    void link_loop() {
        auto backoff = k_first_reconnect;
        while (!stop.load()) {
            std::shared_ptr<net::NativeSocket> socket;
            {
                const std::scoped_lock lock{link_mu};
                socket = link;
            }
            if (socket) {
                for (;;) {
                    auto frame = net::recv_link_frame(*socket);
                    if (!frame || !dispatch(*frame)) {
                        break;
                    }
                }
                drop_link(socket);
                backoff = k_first_reconnect;
            }
            {
                std::unique_lock lock{backoff_mu};
                backoff_cv.wait_for(lock, backoff, [&] { return stop.load(); });
            }
            if (stop.load()) {
                break;
            }
            if (auto reopened = open_link()) {
                set_link(std::move(reopened->first), reopened->second);
            } else {
                backoff = std::min<std::chrono::milliseconds>(backoff * 2,
                                                              std::chrono::milliseconds{options.reconnect_max_ms});
            }
        }
    }

    void audit(const ClientSession& session, std::string_view kind, std::string_view target, const Clock::time_point& start,
               bool ok, ErrorCode code, std::string_view message, std::uint64_t objects, bool denied) {
        policy->audit(AuditRecord{.caller = &session.caller,
                                  .client = session.address,
                                  .kind = kind,
                                  .target = target,
                                  .ok = ok,
                                  .code = code,
                                  .message = message,
                                  .denied = denied,
                                  .duration = Clock::now() - start,
                                  .objects = objects});
    }

    // Uma resposta concluiu um pedido: audita e esquece.
    void complete(ClientSession& session, std::string_view kind, std::uint32_t id, bool ok, ErrorCode code,
                  std::string_view message) {
        std::optional<Pending> done;
        {
            const std::scoped_lock lock{session.pending_mu};
            auto& pending = session.pending_for(kind);
            if (auto found = pending.find(id); found != pending.end()) {
                done = std::move(found->second);
                pending.erase(found);
            }
        }
        if (done) {
            audit(session, done->kind, done->target, done->start, ok, code, message, done->objects, false);
        }
    }

    void writer_loop(ClientSession& session) {
        for (;;) {
            net::Message message;
            {
                std::unique_lock lock{session.out_mu};
                session.out_cv.wait(lock, [&] { return session.out_closed || !session.out.empty(); });
                if (session.out.empty()) {
                    return;
                }
                message = std::move(session.out.front());
                session.out.pop_front();
            }
            policy->on_response(session.caller, message);

            std::optional<std::uint32_t> credit_for;
            if (auto* frame = std::get_if<net::ObjectFrame>(&message)) {
                // O engine manda sem compressão (link local); o codec é o do cliente.
                frame->compression = session.codec;
                credit_for = frame->query_id;
                const std::scoped_lock lock{session.pending_mu};
                if (auto found = session.queries.find(frame->query_id); found != session.queries.end()) {
                    found->second.objects += frame->records.size();
                }
            } else if (const auto* end = std::get_if<net::StreamEnd>(&message)) {
                complete(session, "query", end->query_id, true, ErrorCode::invalid_argument, {});
            } else if (const auto* error = std::get_if<net::StreamError>(&message)) {
                complete(session, "query", error->query_id, false, error->code, error->message);
            } else if (const auto* result = std::get_if<net::OpResult>(&message)) {
                complete(session, "call", result->call_id, result->ok, result->code, result->message);
            } else if (const auto* list = std::get_if<net::FacadeListOk>(&message)) {
                complete(session, "facade_list", list->request_id, true, ErrorCode::invalid_argument, {});
            } else if (const auto* open = std::get_if<net::FacadeOpenOk>(&message)) {
                complete(session, "facade_open", open->request_id, open->ok, open->code, open->message);
            }

            if (auto sent = net::send_message(*session.socket, message); !sent) {
                // O cliente sumiu: a leitora acorda e encerra a sessão.
                static_cast<void>(session.socket->shutdown());
                return;
            }
            // Entregue ao cliente: o stream pode mandar mais um frame.
            if (credit_for) {
                static_cast<void>(send_engine(session.id, net::LinkControl{net::StreamCredit{.query_id = *credit_for, .frames = 1}}));
            }
        }
    }

    [[nodiscard]] std::uint32_t register_session(const std::shared_ptr<ClientSession>& session) {
        const std::scoped_lock lock{sessions_mu};
        while (next_session == net::link_control_session || sessions.contains(next_session)) {
            ++next_session;
        }
        session->id = next_session++;
        sessions.emplace(session->id, session);
        return session->id;
    }

    void unregister_session(std::uint32_t id) {
        const std::scoped_lock lock{sessions_mu};
        sessions.erase(id);
    }

    void handle_client(const std::shared_ptr<net::NativeSocket>& socket) {
        if (small_buffers.load()) {
            static_cast<void>(socket->set_send_buffer_bytes(k_small_socket_buffer));
        }
        auto first = net::recv_message(*socket);
        const auto* hello = first ? std::get_if<net::Hello>(&*first) : nullptr;
        if (hello == nullptr) {
            return;
        }
        net::LinkHelloOk engine_now;
        {
            const std::scoped_lock lock{link_mu};
            if (!link) {
                return;  // sem engine não há o que oferecer
            }
            engine_now = engine;
        }
        auto negotiated = net::negotiate_hello(*hello,
                                               net::HelloOk{.baseline = engine_now.baseline,
                                                            .max_frame_bytes = net::max_frame_bytes,
                                                            .max_concurrent_streams = engine_now.max_concurrent_streams,
                                                            .max_expansion_ratio = net::default_max_expansion_ratio,
                                                            .idle_timeout_ms = options.idle_timeout_ms},
                                               options.preferred_codec);
        if (!negotiated || !net::send_message(*socket, *negotiated)) {
            return;
        }
        if (options.idle_timeout_ms > 0) {
            static_cast<void>(socket->set_recv_timeout_ms(options.idle_timeout_ms));
        }

        const ClientInfo info{.address = socket->peer_address().value_or("?")};
        auto caller = policy->authenticate(info, Credentials{});
        if (!caller) {
            return;
        }

        auto session = std::make_shared<ClientSession>();
        session->socket = socket;
        session->codec = negotiated->selected_codec;
        session->caller = std::move(*caller);
        session->address = info.address;
        const auto id = register_session(session);

        const auto opened = [&]() -> bool {
            if (!send_engine(id, net::LinkControl{net::SessionOpen{.client_minor = negotiated->minor,
                                                                   .principal = session->caller.principal,
                                                                   .roles = session->caller.roles,
                                                                   .attributes = session->caller.attributes}})) {
                return false;
            }
            std::unique_lock lock{session->open_mu};
            session->open_cv.wait_for(lock, k_open_timeout, [&] { return session->open_reply.has_value() || stop.load(); });
            return session->open_reply && session->open_reply->ok;
        }();
        if (!opened) {
            unregister_session(id);
            return;
        }

        std::thread writer{[this, &session] { writer_loop(*session); }};
        for (;;) {
            auto message = net::recv_message(*socket, negotiated->max_frame_bytes, negotiated->max_expansion_ratio);
            if (!message) {
                break;
            }
            // Cancelar o próprio stream é sempre permitido e não espera nada.
            if (std::holds_alternative<net::Cancel>(*message)) {
                if (!send_engine(id, *message)) {
                    break;
                }
                continue;
            }
            if (!is_client_request(*message)) {
                break;  // Hello repetido ou resposta vinda do cliente: fim da conversa
            }
            if (const auto* attach = std::get_if<net::ShmAttach>(&*message)) {
                session->push(net::ShmAttachOk{.request_id = attach->request_id,
                                               .ok = false,
                                               .code = ErrorCode::invalid_argument,
                                               .message = "shared-memory ring is not served by this proxy"});
                continue;
            }
            const auto started = Clock::now();
            auto decision = policy->authorize(session->caller, *message);
            auto key = request_key(*message);
            if (!decision.allowed) {
                if (key) {
                    audit(*session, key->kind, key->target, started, false, decision.code, decision.message, 0, true);
                }
                if (auto reply = denial(*message, decision)) {
                    session->push(std::move(*reply));
                }
                continue;
            }
            if (key) {
                const std::scoped_lock lock{session->pending_mu};
                session->pending_for(key->kind)[key->id] =
                    Pending{.kind = key->kind, .target = std::move(key->target), .start = started};
            }
            if (!send_engine(id, *message)) {
                break;
            }
            if (const auto* query = std::get_if<net::Query>(&*message)) {
                if (!send_engine(id, net::LinkControl{net::StreamCredit{.query_id = query->query_id,
                                                                        .frames = options.stream_credit}})) {
                    break;
                }
            }
        }

        unregister_session(id);
        if (!session->engine_closed.load()) {
            static_cast<void>(send_engine(id, net::LinkControl{net::SessionClose{}}));
        }
        session->close_out();
        writer.join();
    }

    void request_stop() noexcept {
        stop.store(true);
        static_cast<void>(listener.close());
        {
            const std::scoped_lock lock{link_mu};
            if (link) {
                static_cast<void>(link->shutdown());
            }
        }
        backoff_cv.notify_all();
        const std::scoped_lock lock{clients_mu};
        for (auto& [id, socket] : clients) {
            (void)id;
            static_cast<void>(socket->shutdown());
        }
    }

    void join_link() {
        if (link_thread.joinable()) {
            link_thread.join();
        }
    }
};

Proxy::Proxy(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
Proxy::Proxy(Proxy&&) noexcept = default;

Proxy& Proxy::operator=(Proxy&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            impl_->request_stop();
            impl_->join_link();
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Proxy::~Proxy() {
    if (impl_) {
        impl_->request_stop();
        impl_->join_link();
    }
}

Result<Proxy> Proxy::start(Options options, std::shared_ptr<Policy> policy) {
    if (!policy) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "proxy needs a policy"});
    }
    if (options.engine.empty()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "proxy needs the engine socket path"});
    }
    auto impl = std::make_unique<Impl>();
    impl->options = std::move(options);
    impl->policy = std::move(policy);
    impl->options.stream_credit = std::max<std::uint32_t>(1, impl->options.stream_credit);

    auto link = impl->open_link();
    if (!link) {
        return std::unexpected(link.error());
    }
    auto listener = net::NativeSocket::listen(impl->options.host, impl->options.port);
    if (!listener) {
        return std::unexpected(listener.error());
    }
    auto port = listener->local_port();
    if (!port) {
        return std::unexpected(port.error());
    }
    impl->listener = std::move(*listener);
    impl->port = *port;
    impl->set_link(std::move(link->first), link->second);
    impl->link_thread = std::thread{[raw = impl.get()] { raw->link_loop(); }};
    return Proxy{std::move(impl)};
}

std::uint16_t Proxy::port() const noexcept { return impl_ ? impl_->port : 0; }

bool Proxy::link_up() const noexcept { return impl_ && impl_->link_is_up.load(); }

std::size_t Proxy::session_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    const std::scoped_lock lock{impl_->sessions_mu};
    return impl_->sessions.size();
}

void Proxy::use_small_socket_buffers(bool enabled) noexcept {
    if (impl_) {
        impl_->small_buffers.store(enabled);
    }
}

void Proxy::request_stop() noexcept {
    if (impl_) {
        impl_->request_stop();
    }
}

Result<void> Proxy::serve_forever() {
    if (!impl_) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "moved-from proxy"});
    }
    Impl& self = *impl_;
    std::unordered_map<std::uint64_t, std::thread> threads;
    Result<void> status{};
    while (!self.stop.load()) {
        auto peer = self.listener.accept();
        if (!peer) {
            if (!self.stop.load() && self.listener.is_open()) {
                status = std::unexpected(peer.error());
            }
            break;
        }
        auto socket = std::make_shared<net::NativeSocket>(std::move(*peer));
        std::uint64_t client_id = 0;
        std::vector<std::uint64_t> finished;
        {
            const std::scoped_lock lock{self.clients_mu};
            client_id = ++self.next_client;
            self.clients.emplace(client_id, socket);
            if (self.stop.load()) {
                static_cast<void>(socket->shutdown());
            }
            finished.swap(self.finished);
        }
        threads.emplace(client_id, std::thread{[&self, socket, client_id] {
                            self.handle_client(socket);
                            static_cast<void>(socket->shutdown());
                            const std::scoped_lock lock{self.clients_mu};
                            self.clients.erase(client_id);
                            self.finished.push_back(client_id);
                        }});
        // Junta os clientes que já saíram, para a lista não crescer sem fim.
        for (const auto id : finished) {
            if (auto found = threads.find(id); found != threads.end()) {
                found->second.join();
                threads.erase(found);
            }
        }
    }
    self.request_stop();
    for (auto& [id, thread] : threads) {
        (void)id;
        thread.join();
    }
    self.join_link();
    return status;
}

} // namespace modb::proxy
