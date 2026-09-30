// O engine atende um link de proxy (ADR-028, PLANO_PROXY X4).
//
// Um link carrega muitas sessões. A thread do link lê os frames e os despacha
// pela sessão; as mensagens de cada sessão rodam num pool de workers do link,
// uma de cada vez por sessão (as respostas saem na ordem das chamadas) e em
// paralelo entre sessões (uma proc lenta de um cliente não para os outros).
// Os frames de consulta só saem com crédito do stream: num link compartilhado
// a janela TCP não serve de backpressure, e um cliente lento pararia todos.

#include "engine_session.hpp"

#include "modb/net/link_protocol.hpp"
#include "modb/net/server.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace modb::net {
namespace {

// Mensagens de uma sessão que um worker trata antes de dar a vez a outra.
constexpr int k_strand_budget = 16;

Error make_protocol(std::string message) {
    return Error{ErrorCode::protocol_error, std::move(message)};
}

// Compara sem sair cedo: o tempo não diz quantos bytes do segredo acertaram.
[[nodiscard]] bool same_secret(std::string_view expected, std::string_view given) noexcept {
    unsigned char diff = expected.size() == given.size() ? 0 : 1;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const unsigned char other = i < given.size() ? static_cast<unsigned char>(given[i]) : 0;
        diff |= static_cast<unsigned char>(static_cast<unsigned char>(expected[i]) ^ other);
    }
    return diff == 0;
}

// Escrita no link: um frame inteiro por vez, de qualquer thread.
class LinkWriter {
public:
    explicit LinkWriter(NativeSocket& socket) noexcept : socket_{&socket} {}

    [[nodiscard]] Result<void> send(std::uint32_t session, const Message& message) {
        const std::scoped_lock lock{mu_};
        return send_link_message(*socket_, session, message);
    }
    [[nodiscard]] Result<void> send(const LinkFrame& frame) {
        const std::scoped_lock lock{mu_};
        return send_link_frame(*socket_, frame);
    }

private:
    NativeSocket* socket_;
    std::mutex mu_;
};

// A saída de uma sessão do link: o número da sessão em cada frame, e crédito
// por stream para os ObjectFrames.
class LinkSink final : public SessionSink {
public:
    LinkSink(LinkWriter& writer, std::uint32_t session) noexcept : writer_{&writer}, session_{session} {}

    [[nodiscard]] Result<void> send(const Message& message) override {
        if (closed()) {
            return std::unexpected(Error{ErrorCode::connection_closed, "link session closed"});
        }
        return writer_->send(session_, message);
    }

    [[nodiscard]] Result<void> send_stream_frame(const ObjectFrame& frame) override {
        {
            std::unique_lock lock{mu_};
            cv_.wait(lock, [&] { return closed_ || credits_[frame.query_id] > 0; });
            if (closed_) {
                return std::unexpected(Error{ErrorCode::connection_closed, "link session closed"});
            }
            --credits_[frame.query_id];
        }
        return writer_->send(session_, frame);
    }

    // Crédito que sobrou de um stream que acabou. Um crédito que chegue
    // depois (já estava a caminho) cria uma entrada nova, que o fim da
    // sessão descarta.
    void stream_closed(std::uint32_t query_id) noexcept override {
        const std::scoped_lock lock{mu_};
        credits_.erase(query_id);
    }

    void grant(std::uint32_t query_id, std::uint32_t frames) {
        {
            const std::scoped_lock lock{mu_};
            auto& credit = credits_[query_id];
            credit = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(std::uint64_t{credit} + frames, std::numeric_limits<std::uint32_t>::max()));
        }
        cv_.notify_all();
    }

    // Acorda quem espera crédito e recusa envios novos.
    void close() noexcept {
        {
            const std::scoped_lock lock{mu_};
            closed_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        const std::scoped_lock lock{mu_};
        return closed_;
    }

private:
    LinkWriter* writer_;
    std::uint32_t session_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::uint32_t, std::uint32_t> credits_;
    bool closed_{false};
};

struct LinkSession {
    LinkSession(LinkWriter& writer, std::uint32_t session_id) : id{session_id}, sink{writer, session_id} {}

    std::uint32_t id;
    LinkSink sink;
    std::unique_ptr<EngineSession> engine;

    // Fila da sessão. `scheduled` = a sessão está no pool (ou rodando nele);
    // só o worker que a roda a tira de lá.
    std::mutex mu;
    std::deque<Message> inbox;
    bool scheduled{false};
    bool closing{false};
};

// Workers que rodam as sessões prontas do link.
class StrandPool {
public:
    using Runner = std::function<void(const std::shared_ptr<LinkSession>&)>;

    StrandPool(std::size_t workers, Runner run) : run_{std::move(run)} {
        for (std::size_t i = 0; i < workers; ++i) {
            threads_.emplace_back([this] { loop(); });
        }
    }
    StrandPool(const StrandPool&) = delete;
    StrandPool& operator=(const StrandPool&) = delete;
    ~StrandPool() { stop(); }

    void post(std::shared_ptr<LinkSession> session) {
        {
            const std::scoped_lock lock{mu_};
            ready_.push_back(std::move(session));
        }
        cv_.notify_one();
    }

    // Termina o que já está na fila e junta as threads.
    void stop() {
        {
            const std::scoped_lock lock{mu_};
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads_.clear();
    }

private:
    void loop() {
        for (;;) {
            std::shared_ptr<LinkSession> session;
            {
                std::unique_lock lock{mu_};
                cv_.wait(lock, [&] { return stopping_ || !ready_.empty(); });
                if (ready_.empty()) {
                    return;
                }
                session = std::move(ready_.front());
                ready_.pop_front();
            }
            run_(session);
        }
    }

    Runner run_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<LinkSession>> ready_;
    bool stopping_{false};
    std::vector<std::thread> threads_;
};

// Estado de um link: as sessões abertas e quantas ainda não terminaram.
struct LinkState {
    explicit LinkState(NativeSocket& socket) : writer{socket} {}

    LinkWriter writer;
    std::mutex sessions_mu;
    std::unordered_map<std::uint32_t, std::shared_ptr<LinkSession>> sessions;

    std::mutex live_mu;
    std::condition_variable live_cv;
    std::size_t live{0};

    StrandPool* pool{nullptr};

    [[nodiscard]] std::shared_ptr<LinkSession> find(std::uint32_t id) {
        const std::scoped_lock lock{sessions_mu};
        const auto found = sessions.find(id);
        return found == sessions.end() ? nullptr : found->second;
    }

    std::shared_ptr<LinkSession> take(std::uint32_t id) {
        const std::scoped_lock lock{sessions_mu};
        const auto found = sessions.find(id);
        if (found == sessions.end()) {
            return nullptr;
        }
        auto session = std::move(found->second);
        sessions.erase(found);
        return session;
    }

    // Põe a sessão no pool se ela ainda não está lá.
    void schedule(const std::shared_ptr<LinkSession>& session, std::optional<Message> message) {
        bool post = false;
        {
            const std::scoped_lock lock{session->mu};
            if (session->closing) {
                return;
            }
            if (message) {
                session->inbox.push_back(std::move(*message));
            }
            post = !session->scheduled;
            session->scheduled = true;
        }
        if (post) {
            pool->post(session);
        }
    }

    // A sessão vai acabar: descarta o que não começou, acorda os streams que
    // esperam crédito, e o worker dela faz o resto (`finish`).
    void begin_close(const std::shared_ptr<LinkSession>& session) {
        bool post = false;
        {
            const std::scoped_lock lock{session->mu};
            session->closing = true;
            session->inbox.clear();
            post = !session->scheduled;
            session->scheduled = true;
        }
        session->sink.close();
        if (post) {
            pool->post(session);
        }
    }

    void finish(const std::shared_ptr<LinkSession>& session) {
        session->sink.close();
        session->engine->finish();
        {
            const std::scoped_lock lock{live_mu};
            --live;
        }
        live_cv.notify_all();
    }

    void run(const std::shared_ptr<LinkSession>& session) {
        for (int handled = 0; handled < k_strand_budget; ++handled) {
            Message message;
            {
                const std::scoped_lock lock{session->mu};
                if (session->closing) {
                    break;
                }
                if (session->inbox.empty()) {
                    session->scheduled = false;
                    return;
                }
                message = std::move(session->inbox.front());
                session->inbox.pop_front();
            }
            if (auto status = session->engine->handle(message); !status) {
                // O engine encerra a sessão (mensagem que não cabe numa sessão
                // ou link sem saída): avisa o proxy e termina.
                {
                    const std::scoped_lock lock{session->mu};
                    session->closing = true;
                    session->inbox.clear();
                }
                static_cast<void>(take(session->id));
                static_cast<void>(writer.send(LinkFrame{
                    .session = session->id,
                    .body = LinkControl{SessionClose{.code = status.error().code, .message = status.error().message}},
                }));
                finish(session);
                return;
            }
        }
        {
            const std::scoped_lock lock{session->mu};
            if (!session->closing) {
                if (session->inbox.empty()) {
                    session->scheduled = false;
                    return;
                }
                // Gastou a vez com fila ainda cheia: volta para o fim do pool.
                pool->post(session);
                return;
            }
        }
        finish(session);
    }
};

} // namespace

Result<void> Server::handle_link(NativeSocket& peer) {
    // --- abertura do link ---
    auto first = recv_link_frame(peer);
    if (!first) {
        return std::unexpected(first.error());
    }
    const auto* control = std::get_if<LinkControl>(&first->body);
    const auto* hello = control != nullptr ? std::get_if<LinkHello>(control) : nullptr;
    if (hello == nullptr) {
        return std::unexpected(make_protocol("expected LinkHello as first link frame"));
    }
    LinkHelloOk reply{.baseline = baseline_,
                      .database_name = database_name_,
                      .max_concurrent_streams = max_concurrent_streams_};
    if (operations_) {
        for (auto& [id, mode] : operations_->list()) {
            reply.operations.emplace_back(std::move(id), mode == ops::OperationMode::read_only);
        }
    }
    if (hello->version != link_version) {
        reply.ok = false;
        reply.code = ErrorCode::incompatible_protocol_version;
        reply.message = "unsupported link version " + std::to_string(hello->version);
    } else if (!link_secret_.empty() && !same_secret(link_secret_, hello->secret)) {
        reply.ok = false;
        reply.code = ErrorCode::unauthenticated;
        reply.message = "link secret refused";
    }
    if (auto status = send_link_frame(peer, LinkFrame{.session = link_control_session, .body = LinkControl{reply}});
        !status || !reply.ok) {
        (void)peer.close();
        return status ? Result<void>{std::unexpected(Error{reply.code, reply.message})} : status;
    }

    // --- sessões ---
    LinkState state{peer};
    const std::size_t workers =
        link_workers_ != 0 ? link_workers_ : std::max<std::size_t>(2, std::thread::hardware_concurrency());
    StrandPool pool{workers, [&state](const std::shared_ptr<LinkSession>& session) { state.run(session); }};
    state.pool = &pool;

    Result<void> link_status{};
    for (;;) {
        auto frame = recv_link_frame(peer);
        if (!frame) {
            const auto code = frame.error().code;
            if (code != ErrorCode::connection_closed && code != ErrorCode::io_error) {
                link_status = std::unexpected(frame.error());
            }
            break;
        }
        if (frame->session == link_control_session) {
            link_status = std::unexpected(make_protocol("unexpected link control message after LinkHello"));
            break;
        }
        const std::uint32_t id = frame->session;

        if (auto* message = std::get_if<Message>(&frame->body)) {
            auto session = state.find(id);
            if (!session) {
                // Sessão que já fechou (mensagem a caminho) ou nunca abriu.
                static_cast<void>(state.writer.send(LinkFrame{
                    .session = id,
                    .body = LinkControl{SessionClose{.code = ErrorCode::invalid_argument, .message = "unknown session"}},
                }));
                continue;
            }
            // Cancel não espera a fila: a sessão pode estar num OpCall.
            if (!session->engine->handle_urgent(*message)) {
                state.schedule(session, std::move(*message));
            }
            continue;
        }

        auto& link_control = std::get<LinkControl>(frame->body);
        if (auto* open = std::get_if<SessionOpen>(&link_control)) {
            if (state.find(id)) {
                static_cast<void>(state.writer.send(LinkFrame{
                    .session = id,
                    .body = LinkControl{SessionOpenOk{.ok = false,
                                                      .code = ErrorCode::invalid_argument,
                                                      .message = "session already open"}},
                }));
                continue;
            }
            auto services = session_services();
            services.allow_shm = false;
            services.caller = ops::Caller{.principal = std::move(open->principal),
                                          .roles = std::move(open->roles),
                                          .attributes = std::move(open->attributes)};
            services.client_minor = open->client_minor;
            services.delegation_allowed = true;
            auto session = std::make_shared<LinkSession>(state.writer, id);
            // Sem compressão no link: é local; quem comprime para o cliente é o proxy.
            session->engine = std::make_unique<EngineSession>(std::move(services), session->sink, Compression::none);
            {
                const std::scoped_lock lock{state.live_mu};
                ++state.live;
            }
            {
                const std::scoped_lock lock{state.sessions_mu};
                state.sessions.emplace(id, session);
            }
            if (auto status = state.writer.send(LinkFrame{.session = id, .body = LinkControl{SessionOpenOk{}}});
                !status) {
                link_status = std::unexpected(status.error());
                break;
            }
            continue;
        }
        if (std::holds_alternative<SessionClose>(link_control)) {
            if (auto session = state.take(id)) {
                state.begin_close(session);
            }
            continue;
        }
        if (const auto* credit = std::get_if<StreamCredit>(&link_control)) {
            if (auto session = state.find(id)) {
                session->sink.grant(credit->query_id, credit->frames);
            }
            continue;
        }
        link_status = std::unexpected(make_protocol("unexpected link message from the proxy"));
        break;
    }

    // --- fim do link: nada mais sai; as sessões terminam e o pool para ---
    (void)peer.shutdown();
    std::vector<std::shared_ptr<LinkSession>> remaining;
    {
        const std::scoped_lock lock{state.sessions_mu};
        for (auto& [id, session] : state.sessions) {
            (void)id;
            remaining.push_back(std::move(session));
        }
        state.sessions.clear();
    }
    for (const auto& session : remaining) {
        state.begin_close(session);
    }
    {
        std::unique_lock lock{state.live_mu};
        state.live_cv.wait(lock, [&] { return state.live == 0; });
    }
    pool.stop();
    (void)peer.close();
    return link_status;
}

} // namespace modb::net
