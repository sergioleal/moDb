#include "modb/proxy/proxy.hpp"

#include "modb/net/link_protocol.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/net/server.hpp"
#include "modb/net/shm_ring.hpp"

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
    // O chamador efetivo do pedido (com o delegado, se houver), para a auditoria.
    ops::Caller caller{};
};

// Delegação (ADR-029): um principal fala por outro só com esta role.
constexpr std::string_view k_delegate_role = "delegate";

// Quem o pedido representa. A regra fica no proxy, fora da cadeia de políticas:
// nenhuma política configurada a desliga. Devolve a recusa, ou nada e o
// chamador efetivo (o da sessão mais o delegado) em `effective`.
[[nodiscard]] std::optional<Decision> check_delegation(const ops::Caller& caller, const net::Message& request,
                                                       std::uint16_t engine_minor, ops::Caller& effective) {
    effective = caller;
    const auto* delegation = net::request_delegation(request);
    const auto* call = std::get_if<net::OpCall>(&request);
    const bool keyed = call != nullptr && !call->idempotency_key.empty();
    if ((delegation != nullptr || keyed) && engine_minor < 3) {
        return Decision::deny("the engine does not support protocol minor 3 (delegation, idempotency keys)",
                              ErrorCode::invalid_argument);
    }
    if (delegation == nullptr) {
        return std::nullopt;
    }
    // O delegado entra no chamador efetivo antes da recusa: a auditoria mostra
    // em nome de quem tentaram falar.
    effective.acting_as = delegation->subject;
    effective.acting_attributes = delegation->attributes;
    if (!caller.has_role(k_delegate_role)) {
        return Decision::deny("acting_as requires the 'delegate' role", ErrorCode::permission_denied);
    }
    return std::nullopt;
}

// O que só o cliente manda numa sessão (o resto é resposta ou abertura).
[[nodiscard]] bool is_client_request(const net::Message& message) noexcept {
    return std::holds_alternative<net::Query>(message) || std::holds_alternative<net::Cancel>(message) ||
           std::holds_alternative<net::OpCall>(message) || std::holds_alternative<net::FacadeList>(message) ||
           std::holds_alternative<net::FacadeOpen>(message) || std::holds_alternative<net::ShmAttach>(message) ||
           std::holds_alternative<net::Authenticate>(message);
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
    if (const auto* attach = std::get_if<net::ShmAttach>(&request)) {
        return net::ShmAttachOk{.request_id = attach->request_id,
                                .ok = false,
                                .code = decision.code,
                                .message = decision.message};
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
        return RequestKey{"query", query->query_id, std::to_string(query->description.type.value)};
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
    // Minor negociado com o cliente, e o do engine quando a sessão abriu.
    std::uint16_t client_minor{0};
    std::uint16_t engine_minor{0};

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

    // Anel de memória compartilhada (ADR-026, X11). O anel de um cliente tem
    // uma sessão própria no engine, com o mesmo chamador: `ring` aponta para a
    // região nela, e as respostas saem pelo anel, não pelo socket.
    net::shm::Region* ring{nullptr};
    std::atomic<bool> ring_stop{false};
    // Na sessão do cliente: a sessão do anel, a região e as threads dele.
    std::shared_ptr<ClientSession> shm_session;
    std::unique_ptr<net::shm::Region> shm_region;
    std::thread shm_reader;
    std::thread shm_writer;

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
        // A política vê o catálogo antes do primeiro cliente deste link.
        EngineInfo info{.database_name = hello.database_name, .baseline = hello.baseline.value};
        for (const auto& [id, read_only] : hello.operations) {
            info.operations.push_back(EngineInfo::Operation{.id = id, .read_only = read_only});
        }
        std::sort(info.operations.begin(), info.operations.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
        policy->on_engine(info);
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

    void audit(const ClientSession& session, const ops::Caller& caller, std::string_view kind, std::string_view target,
               const Clock::time_point& start, bool ok, ErrorCode code, std::string_view message, std::uint64_t objects,
               bool denied) {
        policy->audit(AuditRecord{.caller = &caller,
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
            audit(session, done->caller, done->kind, done->target, done->start, ok, code, message, done->objects, false);
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
            // O detail do erro (minor 3) só vai a quem negociou minor ≥ 3.
            if (auto* result = std::get_if<net::OpResult>(&message); result != nullptr && session.client_minor < 3) {
                result->detail.clear();
            }

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

            if (session.ring != nullptr) {
                if (!write_ring(session, message)) {
                    return;
                }
                continue;
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

    [[nodiscard]] static bool ring_stopping(const ClientSession& ring_session) {
        return ring_session.ring_stop.load(std::memory_order_relaxed) ||
               ring_session.ring->load_state(net::shm::k_off_client_state) ==
                   static_cast<std::uint32_t>(net::shm::ClientState::leaving);
    }

    // Uma resposta no anel de respostas; espera o cliente abrir espaço.
    // false = o anel acabou (cliente saiu ou a sessão fechou).
    [[nodiscard]] bool write_ring(ClientSession& ring_session, const net::Message& message) {
        auto responses = net::shm::Ring::responses(*ring_session.ring);
        auto bytes = net::encode_message(message);
        if (bytes && bytes->size() + 7 > responses.capacity()) {
            const auto* result = std::get_if<net::OpResult>(&message);
            bytes = net::encode_message(net::OpResult{
                .call_id = result != nullptr ? result->call_id : 0,
                .ok = false,
                .code = ErrorCode::value_too_large,
                .message = "result does not fit the shared-memory ring; call over TCP or attach a larger ring"});
        }
        if (!bytes) {
            return false;
        }
        net::shm::Backoff full;
        for (;;) {
            auto written = responses.try_write(*bytes);
            if (!written) {
                return false;
            }
            if (*written) {
                return true;
            }
            if (ring_stopping(ring_session) || stop.load()) {
                return false;
            }
            full.wait();  // cliente atrasado em consumir as respostas
        }
    }

    // Lê os pedidos do anel, passa cada um pela política e manda os
    // permitidos ao engine pela sessão do anel.
    void ring_loop(ClientSession& owner) {
        auto& ring_session = *owner.shm_session;
        auto& region = *owner.shm_region;
        auto requests = net::shm::Ring::requests(region);
        net::shm::Backoff idle;
        while (!ring_stopping(ring_session) && !stop.load() && !ring_session.engine_closed.load()) {
            // O cliente já mapeou: o nome pode sair do sistema de arquivos.
            if (region.load_state(net::shm::k_off_client_state) ==
                static_cast<std::uint32_t>(net::shm::ClientState::attached)) {
                region.unlink();
            }
            auto next = requests.peek();
            if (!next) {
                break;  // anel corrompido: encerra o anel (a sessão TCP segue)
            }
            if (!next->has_value()) {
                idle.wait();
                continue;
            }
            idle.reset();
            auto message = net::decode_message(**next);
            requests.pop();
            auto* call = message ? std::get_if<net::OpCall>(&*message) : nullptr;
            if (call == nullptr) {
                ring_session.push(net::OpResult{.ok = false,
                                                .code = ErrorCode::protocol_error,
                                                .message = "only OpCall travels over the shared-memory ring"});
                continue;
            }
            const auto started = Clock::now();
            ops::Caller effective;
            auto decision = Decision::allow();
            if (auto refused = check_delegation(ring_session.caller, *message, ring_session.engine_minor, effective)) {
                decision = std::move(*refused);
            } else {
                decision = policy->authorize(effective, *message);
            }
            call = std::get_if<net::OpCall>(&*message);
            if (!decision.allowed || call == nullptr) {
                audit(ring_session, effective, "call", call != nullptr ? call->operation_id : std::string{}, started,
                      false, decision.code, decision.message, 0, true);
                ring_session.push(net::OpResult{.call_id = call != nullptr ? call->call_id : 0,
                                                .ok = false,
                                                .code = decision.code,
                                                .message = decision.message});
                continue;
            }
            {
                const std::scoped_lock lock{ring_session.pending_mu};
                ring_session.calls[call->call_id] =
                    Pending{.kind = "call", .target = call->operation_id, .start = started, .caller = effective};
            }
            if (!send_engine(ring_session.id, *message)) {
                ring_session.push(net::OpResult{.call_id = call->call_id,
                                                .ok = false,
                                                .code = ErrorCode::connection_closed,
                                                .message = "link to the engine is down"});
                break;
            }
        }
        region.store_state(net::shm::k_off_server_state, static_cast<std::uint32_t>(net::shm::ServerState::closed));
    }

    // ShmAttach do cliente: cria a região e a sessão do anel no engine.
    void attach_ring(const std::shared_ptr<ClientSession>& owner, const net::ShmAttach& attach,
                     std::uint16_t client_minor) {
        net::ShmAttachOk reply{.request_id = attach.request_id};
        const auto refuse = [&](ErrorCode code, std::string message) {
            reply.ok = false;
            reply.code = code;
            reply.message = std::move(message);
            owner->push(reply);
        };
        if (owner->shm_session) {
            refuse(ErrorCode::invalid_argument, "session already has a ring");
            return;
        }
        auto region = net::shm::Region::create(attach.ring_bytes);
        if (!region) {
            refuse(region.error().code, region.error().message);
            return;
        }
        auto ring_session = std::make_shared<ClientSession>();
        ring_session->socket = owner->socket;
        ring_session->caller = owner->caller;
        ring_session->address = owner->address;
        ring_session->client_minor = owner->client_minor;
        ring_session->engine_minor = owner->engine_minor;
        owner->shm_region = std::make_unique<net::shm::Region>(std::move(*region));
        ring_session->ring = owner->shm_region.get();
        if (!open_session(ring_session, client_minor)) {
            owner->shm_region.reset();
            refuse(ErrorCode::connection_closed, "engine did not open the ring session");
            return;
        }
        owner->shm_session = ring_session;
        owner->shm_writer = std::thread{[this, ring_session] { writer_loop(*ring_session); }};
        owner->shm_reader = std::thread{[this, owner] { ring_loop(*owner); }};
        reply.kind = owner->shm_region->kind();
        reply.name = owner->shm_region->name();
        reply.ring_bytes = owner->shm_region->ring_bytes();
        owner->push(reply);
    }

    // Fecha uma sessão no engine e audita o que ficou sem resposta.
    void close_session(ClientSession& session) {
        unregister_session(session.id);
        if (!session.engine_closed.load()) {
            static_cast<void>(send_engine(session.id, net::LinkControl{net::SessionClose{}}));
        }
        std::vector<Pending> abandoned;
        {
            const std::scoped_lock lock{session.pending_mu};
            for (auto* pending : {&session.calls, &session.queries, &session.facades}) {
                for (auto& [request_id, entry] : *pending) {
                    (void)request_id;
                    abandoned.push_back(std::move(entry));
                }
                pending->clear();
            }
        }
        // A auditoria fecha a conta de cada pedido (e os limites por
        // principal dependem disso).
        for (const auto& entry : abandoned) {
            audit(session, entry.caller, entry.kind, entry.target, entry.start, false, ErrorCode::connection_closed,
                  "client left before the answer", entry.objects, false);
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

    // Registra a sessão e a abre no engine com o chamador já conhecido.
    [[nodiscard]] bool open_session(const std::shared_ptr<ClientSession>& session, std::uint16_t client_minor) {
        const auto id = register_session(session);
        const auto opened = [&]() -> bool {
            if (!send_engine(id, net::LinkControl{net::SessionOpen{.client_minor = client_minor,
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
        }
        return opened;
    }

    // Antes do Authenticate, os pedidos voltam com `unauthenticated` (um
    // cliente antigo, sem Authenticate, recebe erros claros em vez de uma
    // conexão fechada). Três credenciais recusadas fecham a conexão.
    [[nodiscard]] bool authenticate_client(const std::shared_ptr<ClientSession>& session, const ClientInfo& info,
                                           std::uint16_t client_minor, std::uint32_t frame_limit,
                                           std::uint16_t expansion) {
        constexpr int k_attempts = 3;
        auto& socket = *session->socket;
        std::string mechanisms;
        for (const auto& mechanism : policy->mechanisms()) {
            mechanisms += (mechanisms.empty() ? "" : ", ") + mechanism;
        }
        for (int refused = 0; refused < k_attempts;) {
            auto message = net::recv_message(socket, frame_limit, expansion);
            if (!message) {
                return false;
            }
            if (std::holds_alternative<net::Cancel>(*message)) {
                continue;
            }
            if (!is_client_request(*message)) {
                return false;
            }
            const auto* auth = std::get_if<net::Authenticate>(&*message);
            if (auth == nullptr) {
                const auto decision = Decision::deny("authenticate first (mechanisms: " + mechanisms + ")",
                                                     ErrorCode::unauthenticated);
                if (auto reply = denial(*message, decision); reply && !net::send_message(socket, *reply)) {
                    return false;
                }
                continue;
            }
            const auto started = Clock::now();
            auto caller = policy->authenticate(info, Credentials{.mechanism = auth->mechanism, .payload = auth->payload});
            if (!caller) {
                ++refused;
                audit(*session, session->caller, "authenticate", auth->mechanism, started, false, caller.error().code,
                      caller.error().message, 0, true);
                if (!net::send_message(socket, net::AuthenticateOk{.request_id = auth->request_id,
                                                                   .ok = false,
                                                                   .code = caller.error().code,
                                                                   .message = caller.error().message})) {
                    return false;
                }
                continue;
            }
            session->caller = std::move(*caller);
            if (!open_session(session, client_minor)) {
                static_cast<void>(net::send_message(socket, net::AuthenticateOk{.request_id = auth->request_id,
                                                                                .ok = false,
                                                                                .code = ErrorCode::connection_closed,
                                                                                .message = "engine unavailable"}));
                return false;
            }
            audit(*session, session->caller, "authenticate", auth->mechanism, started, true, ErrorCode::invalid_argument,
                  {}, 0, false);
            if (!net::send_message(socket, net::AuthenticateOk{.request_id = auth->request_id,
                                                               .principal = session->caller.principal})) {
                unregister_session(session->id);
                static_cast<void>(send_engine(session->id, net::LinkControl{net::SessionClose{}}));
                return false;
            }
            return true;
        }
        return false;
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
        if (negotiated) {
            negotiated->auth_mechanisms = policy->mechanisms();
            // O cliente não pode usar o que o engine atrás do proxy não entende.
            negotiated->minor = std::min(negotiated->minor, engine_now.protocol_minor);
        }
        if (!negotiated || !net::send_message(*socket, *negotiated)) {
            return;
        }
        if (options.idle_timeout_ms > 0) {
            static_cast<void>(socket->set_recv_timeout_ms(options.idle_timeout_ms));
        }

        const ClientInfo info{.address = socket->peer_address().value_or("?")};
        auto session = std::make_shared<ClientSession>();
        session->socket = socket;
        session->codec = negotiated->selected_codec;
        session->address = info.address;
        session->client_minor = negotiated->minor;
        session->engine_minor = engine_now.protocol_minor;
        const auto frame_limit = negotiated->max_frame_bytes;
        const auto expansion = negotiated->max_expansion_ratio;

        if (negotiated->auth_mechanisms.empty()) {
            auto caller = policy->authenticate(info, Credentials{});
            if (!caller) {
                return;
            }
            session->caller = std::move(*caller);
            if (!open_session(session, negotiated->minor)) {
                return;
            }
        } else if (!authenticate_client(session, info, negotiated->minor, frame_limit, expansion)) {
            return;
        }
        const auto id = session->id;

        std::thread writer{[this, &session] { writer_loop(*session); }};
        for (;;) {
            auto message = net::recv_message(*socket, frame_limit, expansion);
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
            if (const auto* auth = std::get_if<net::Authenticate>(&*message)) {
                session->push(net::AuthenticateOk{.request_id = auth->request_id,
                                                  .ok = false,
                                                  .code = ErrorCode::invalid_argument,
                                                  .message = "already authenticated"});
                continue;
            }
            if (const auto* attach = std::get_if<net::ShmAttach>(&*message)) {
                attach_ring(session, *attach, negotiated->minor);
                continue;
            }
            const auto started = Clock::now();
            ops::Caller effective;
            auto decision = Decision::allow();
            if (auto refused = check_delegation(session->caller, *message, session->engine_minor, effective)) {
                decision = std::move(*refused);
            } else {
                decision = policy->authorize(effective, *message);
            }
            auto key = request_key(*message);
            if (!decision.allowed) {
                if (key) {
                    audit(*session, effective, key->kind, key->target, started, false, decision.code, decision.message,
                          0, true);
                }
                if (auto reply = denial(*message, decision)) {
                    session->push(std::move(*reply));
                }
                continue;
            }
            if (key) {
                const std::scoped_lock lock{session->pending_mu};
                session->pending_for(key->kind)[key->id] =
                    Pending{.kind = key->kind, .target = std::move(key->target), .start = started, .caller = effective};
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

        // O anel primeiro: a leitora dele usa a região da sessão.
        if (session->shm_session) {
            auto& ring_session = *session->shm_session;
            ring_session.ring_stop.store(true);
            session->shm_reader.join();
            close_session(ring_session);
            ring_session.close_out();
            session->shm_writer.join();
        }
        close_session(*session);
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
