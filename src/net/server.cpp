#include "modb/net/server.hpp"

#include "modb/compatibility.hpp"
#include "modb/object/object_codec.hpp"
#include "modb/storage/endian.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace modb::net {
namespace {

Error make_protocol(std::string message) {
    return Error{ErrorCode::protocol_error, std::move(message)};
}

constexpr std::size_t k_small_socket_buffer = 4 * 1024;

struct SessionState {
    NativeSocket* peer{nullptr};
    std::mutex write_mu;
    std::mutex inbox_mu;
    std::condition_variable inbox_cv;
    std::deque<Message> inbox;
    std::unordered_map<std::uint32_t, query::CancellationToken> tokens;
    std::mutex tokens_mu;
    std::atomic<bool> stop{false};
    std::atomic<bool> reader_failed{false};
    Error reader_error{ErrorCode::connection_closed, "reader stopped"};
};

[[nodiscard]] Result<void> send_locked(SessionState& session, const Message& message) {
    const std::scoped_lock lock{session.write_mu};
    return send_message(*session.peer, message);
}

void reader_loop(SessionState& session) {
    while (!session.stop.load(std::memory_order_relaxed)) {
        auto message = recv_message(*session.peer);
        if (!message) {
            session.reader_failed.store(true, std::memory_order_relaxed);
            session.reader_error = message.error();
            session.stop.store(true, std::memory_order_relaxed);
            session.inbox_cv.notify_all();
            return;
        }
        if (const auto* cancel = std::get_if<Cancel>(&*message)) {
            const std::scoped_lock lock{session.tokens_mu};
            if (auto found = session.tokens.find(cancel->query_id); found != session.tokens.end()) {
                found->second.cancel();
            }
            continue;
        }
        {
            const std::scoped_lock lock{session.inbox_mu};
            session.inbox.push_back(std::move(*message));
        }
        session.inbox_cv.notify_all();
    }
}

[[nodiscard]] Result<Message> wait_inbound(SessionState& session) {
    std::unique_lock lock{session.inbox_mu};
    session.inbox_cv.wait(lock, [&] {
        return session.stop.load(std::memory_order_relaxed) || !session.inbox.empty();
    });
    if (!session.inbox.empty()) {
        Message message = std::move(session.inbox.front());
        session.inbox.pop_front();
        return message;
    }
    if (session.reader_failed.load(std::memory_order_relaxed)) {
        return std::unexpected(session.reader_error);
    }
    return std::unexpected(Error{ErrorCode::connection_closed, "session stopped"});
}

} // namespace

Result<void> send_message(NativeSocket& socket, const Message& message) {
    auto encoded = encode_message(message);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return socket.send_all(*encoded);
}

Result<Message> recv_message(NativeSocket& socket) {
    return recv_message(socket, max_frame_bytes, default_max_expansion_ratio);
}

Result<Message> recv_message(NativeSocket& socket, std::uint32_t negotiated_max_frame,
                             std::uint16_t max_expansion_ratio) {
    std::array<std::byte, 4> length_bytes{};
    if (auto status = socket.recv_exact(length_bytes); !status) {
        return std::unexpected(status.error());
    }
    const auto length =
        storage::load_le<std::uint32_t>(std::span<const std::byte>{length_bytes});
    if (length == 0) {
        return std::unexpected(make_protocol("frame length is zero"));
    }
    if (length > negotiated_max_frame || length > max_frame_bytes) {
        return std::unexpected(
            Error{ErrorCode::frame_too_large, "frame length exceeds negotiated max"});
    }

    std::vector<std::byte> frame(4u + length);
    std::copy(length_bytes.begin(), length_bytes.end(), frame.begin());
    if (auto status = socket.recv_exact(std::span<std::byte>{frame.data() + 4, length}); !status) {
        return std::unexpected(status.error());
    }
    return decode_message(frame, negotiated_max_frame, max_expansion_ratio);
}

Server::Server(std::shared_ptr<object::Database> database, object::DatabaseId database_id,
               NativeSocket listener, std::uint16_t port, std::string database_name,
               object::BaselineId baseline)
    : database_{std::move(database)}, database_id_{database_id}, listener_{std::move(listener)},
      port_{port}, database_name_{std::move(database_name)}, baseline_{baseline} {}

Server::Server(Server&& other) noexcept
    : database_{std::move(other.database_)}, database_id_{other.database_id_},
      listener_{std::move(other.listener_)}, port_{other.port_},
      database_name_{std::move(other.database_name_)}, baseline_{other.baseline_},
      fail_after_{other.fail_after_}, small_buffers_{other.small_buffers_},
      last_stats_{other.last_stats_},
      stop_requested_{other.stop_requested_.load()} {
    other.database_id_ = object::DatabaseId{};
    other.fail_after_.reset();
    other.small_buffers_ = false;
    other.last_stats_ = {};
    other.stop_requested_.store(false);
}

Server::~Server() {
    if (database_id_.value != 0) {
        object::DatabaseRegistry::instance().detach(database_id_);
        database_id_ = object::DatabaseId{};
    }
}

Result<Server> Server::listen(const std::filesystem::path& path, std::string_view host,
                              std::uint16_t port) {
    Result<object::Database> opened = object::Database::open(path);
    if (!opened) {
        if (opened.error().code == ErrorCode::file_not_found) {
            auto created = object::Database::create(path);
            if (!created) {
                return std::unexpected(created.error());
            }
            opened.emplace(std::move(*created));
        } else {
            return std::unexpected(opened.error());
        }
    }

    auto database = std::make_shared<object::Database>(std::move(*opened));
    auto database_id = object::DatabaseRegistry::instance().attach(database);
    if (!database_id) {
        return std::unexpected(database_id.error());
    }

    object::BaselineId baseline{};
    if (const auto& current = database->current_baseline()) {
        baseline = current->id();
    }

    auto listener = NativeSocket::listen(host, port);
    if (!listener) {
        object::DatabaseRegistry::instance().detach(*database_id);
        return std::unexpected(listener.error());
    }
    auto bound_port = listener->local_port();
    if (!bound_port) {
        object::DatabaseRegistry::instance().detach(*database_id);
        return std::unexpected(bound_port.error());
    }

    return Server{std::move(database), *database_id, std::move(*listener), *bound_port,
                  path.filename().string(), baseline};
}

namespace {

Result<StreamStats> run_query(SessionState& session, std::shared_ptr<object::Database> database,
                              const Query& query, std::optional<std::size_t> fail_after,
                              Compression preferred_codec) {
    StreamStats stats{};
    query::CancellationToken token;
    {
        const std::scoped_lock lock{session.tokens_mu};
        session.tokens[query.query_id] = token;
    }

    if (auto status = send_locked(session, StreamBegin{.query_id = query.query_id}); !status) {
        const std::scoped_lock lock{session.tokens_mu};
        session.tokens.erase(query.query_id);
        return std::unexpected(status.error());
    }

    object::Database::ObjectQuerySpec spec{
        .type = query.description.type,
        .limit = query.description.limit,
        .project = query.description.project,
        .cancel = token,
        .has_cancel = true,
    };
    if (query.description.equals) {
        spec.equals = std::pair{query.description.equals->field, query.description.equals->value};
    }

    ObjectFrame batch{.query_id = query.query_id, .compression = preferred_codec};
    bool cancelled = false;

    auto note_outstanding = [&] {
        const auto outstanding = stats.produced - stats.sent;
        if (outstanding > stats.max_outstanding) {
            stats.max_outstanding = outstanding;
        }
    };

    auto flush_batch = [&]() -> Result<void> {
        if (batch.records.empty()) {
            return {};
        }
        const auto count = batch.records.size();
        if (auto status = send_locked(session, batch); !status) {
            return status;
        }
        stats.sent += count;
        note_outstanding();
        batch.records.clear();
        return {};
    };

    for (auto& item : database->query_objects(std::move(spec))) {
        if (token.cancelled()) {
            cancelled = true;
            break;
        }
        if (!item) {
            (void)flush_batch();
            (void)send_locked(session, StreamError{.query_id = query.query_id,
                                                   .code = item.error().code,
                                                   .message = item.error().message});
            const std::scoped_lock lock{session.tokens_mu};
            session.tokens.erase(query.query_id);
            return stats;
        }

        if (fail_after && stats.produced >= *fail_after) {
            (void)flush_batch();
            (void)send_locked(session, StreamError{.query_id = query.query_id,
                                                   .code = ErrorCode::io_error,
                                                   .message = "injected stream failure"});
            const std::scoped_lock lock{session.tokens_mu};
            session.tokens.erase(query.query_id);
            return stats;
        }

        auto payload = object::encode_object_payload(item->fields);
        if (!payload) {
            (void)flush_batch();
            (void)send_locked(session, StreamError{.query_id = query.query_id,
                                                   .code = payload.error().code,
                                                   .message = payload.error().message});
            const std::scoped_lock lock{session.tokens_mu};
            session.tokens.erase(query.query_id);
            return stats;
        }

        batch.records.push_back(ObjectEnvelope{
            .object_id = item->id,
            .type_definition_id = item->type,
            .payload = std::move(*payload),
        });
        ++stats.produced;
        note_outstanding();

        if (batch.records.size() >= max_in_flight_objects) {
            if (auto status = flush_batch(); !status) {
                const std::scoped_lock lock{session.tokens_mu};
                session.tokens.erase(query.query_id);
                return std::unexpected(status.error());
            }
        }
    }

    if (auto status = flush_batch(); !status) {
        const std::scoped_lock lock{session.tokens_mu};
        session.tokens.erase(query.query_id);
        return std::unexpected(status.error());
    }
    // Cancel ou fim natural: StreamEnd com o total produzido (conexão reutilizável).
    (void)cancelled;
    if (auto status =
            send_locked(session, StreamEnd{.query_id = query.query_id, .total = stats.produced});
        !status) {
        const std::scoped_lock lock{session.tokens_mu};
        session.tokens.erase(query.query_id);
        return std::unexpected(status.error());
    }

    {
        const std::scoped_lock lock{session.tokens_mu};
        session.tokens.erase(query.query_id);
    }
    return stats;
}

} // namespace

Result<void> Server::handle_connection(NativeSocket peer) {
    if (small_buffers_) {
        (void)peer.set_send_buffer_bytes(k_small_socket_buffer);
    }

    auto message = recv_message(peer);
    if (!message) {
        return std::unexpected(message.error());
    }
    const auto* hello = std::get_if<Hello>(&*message);
    if (hello == nullptr) {
        return std::unexpected(make_protocol("expected Hello as first message"));
    }
    if (hello->version == 0) {
        return std::unexpected(Error{ErrorCode::incompatible_protocol_version,
                                     "protocol major must not be zero"});
    }
    auto negotiated = modb::negotiate_protocol_version(
        modb::CompatibilityVersion{hello->version, hello->minor},
        modb::CompatibilityVersion{protocol_major, protocol_minor});
    if (!negotiated) {
        return std::unexpected(negotiated.error());
    }
    const bool accepts_none =
        std::find(hello->accepted_codecs.begin(), hello->accepted_codecs.end(),
                  Compression::none) != hello->accepted_codecs.end();
    if (!accepts_none) {
        return std::unexpected(make_protocol("client must accept compression=none"));
    }

    Compression selected = Compression::none;
    if (preferred_codec_ != Compression::none &&
        std::find(hello->accepted_codecs.begin(), hello->accepted_codecs.end(),
                  preferred_codec_) != hello->accepted_codecs.end()) {
        selected = preferred_codec_;
    }
    selected_codec_ = selected;
    (void)database_name_;

    HelloOk ok{.version = negotiated->major,
               .minor = negotiated->minor,
               .baseline = baseline_,
               .selected_codec = selected,
               .max_frame_bytes = max_frame_bytes,
               .max_concurrent_streams = max_concurrent_streams_,
               .max_expansion_ratio = default_max_expansion_ratio,
               .idle_timeout_ms = idle_timeout_ms_};
    if (auto status = send_message(peer, ok); !status) {
        return status;
    }

    if (idle_timeout_ms_ > 0) {
        if (auto status = peer.set_recv_timeout_ms(idle_timeout_ms_); !status) {
            return status;
        }
    }

    SessionState session;
    session.peer = &peer;
    std::thread reader{[&session] { reader_loop(session); }};

    // Workers ativos para multiplexação (query_id → thread).
    std::mutex workers_mu;
    std::vector<std::thread> workers;
    std::atomic<int> live_workers{0};
    std::mutex stats_mu;

    Result<void> session_status{};
    while (!session.stop.load(std::memory_order_relaxed)) {
        auto inbound = wait_inbound(session);
        if (!inbound) {
            // Peer fechou o socket (FIN ou RST/WSAECONNRESET no Windows após
            // closesocket com recv pendente). Sessão encerra sem erro.
            // Timeout de idle também chega como io_error.
            const auto code = inbound.error().code;
            if (code == ErrorCode::connection_closed || code == ErrorCode::io_error) {
                session_status = {};
            } else {
                session_status = std::unexpected(inbound.error());
            }
            break;
        }
        const auto* query = std::get_if<Query>(&*inbound);
        if (query != nullptr) {
            Query query_copy = *query;
            if (live_workers.load(std::memory_order_relaxed) >=
                static_cast<int>(max_concurrent_streams_)) {
                (void)send_locked(session,
                                  StreamError{.query_id = query_copy.query_id,
                                              .code = ErrorCode::invalid_argument,
                                              .message = "max concurrent streams exceeded"});
                continue;
            }

            live_workers.fetch_add(1, std::memory_order_relaxed);
            const Compression codec = selected_codec_;
            std::thread worker([&, query_copy, codec]() mutable {
                auto stats = run_query(session, database_, query_copy, fail_after_, codec);
                if (stats) {
                    const std::scoped_lock lock{stats_mu};
                    last_stats_ = *stats;
                }
                live_workers.fetch_sub(1, std::memory_order_relaxed);
            });
            {
                const std::scoped_lock lock{workers_mu};
                workers.push_back(std::move(worker));
            }
            continue;
        }

        if (const auto* list = std::get_if<FacadeList>(&*inbound); list != nullptr) {
            FacadeListOk reply{.request_id = list->request_id};
            if (facades_) {
                const auto listed = facades_->list();
                reply.facades.assign(listed.begin(), listed.end());
            }
            if (auto status = send_locked(session, reply); !status) {
                session_status = std::unexpected(status.error());
                break;
            }
            continue;
        }

        if (const auto* open = std::get_if<FacadeOpen>(&*inbound); open != nullptr) {
            FacadeOpenOk reply{.request_id = open->request_id,
                               .facade_id = open->facade_id,
                               .facade_version = open->facade_version};
            if (!facades_) {
                reply.ok = false;
                reply.code = ErrorCode::facade_not_found;
                reply.message = "server has no facade catalog";
            } else {
                auto found = facades_->find(open->facade_id, open->facade_version);
                if (found) {
                    reply.ok = true;
                    reply.facade_id = found->facade_id;
                    reply.facade_version = found->facade_version;
                } else {
                    reply.ok = false;
                    reply.code = found.error().code;
                    reply.message = found.error().message;
                }
            }
            if (auto status = send_locked(session, reply); !status) {
                session_status = std::unexpected(status.error());
                break;
            }
            continue;
        }

        const auto* call = std::get_if<OpCall>(&*inbound);
        if (call == nullptr) {
            session_status =
                std::unexpected(make_protocol("expected Query, OpCall, or Facade* in session"));
            break;
        }

        OpResult reply{.call_id = call->call_id};
        if (!operations_) {
            reply.ok = false;
            reply.code = ErrorCode::operation_not_found;
            reply.message = "server has no operation registry";
        } else {
            auto outcome = operations_->dispatch(call->operation_id, call->args, *database_);
            if (outcome) {
                reply.ok = true;
                reply.payload = std::move(outcome->payload);
            } else {
                reply.ok = false;
                reply.code = outcome.error().code;
                reply.message = outcome.error().message;
            }
        }
        if (auto status = send_locked(session, reply); !status) {
            session_status = std::unexpected(status.error());
            break;
        }
    }

    session.stop.store(true, std::memory_order_relaxed);
    // Cancela consultas ativas para os workers saírem do generator.
    {
        const std::scoped_lock lock{session.tokens_mu};
        for (auto& [id, token] : session.tokens) {
            (void)id;
            token.cancel();
        }
    }
    // Junta workers ANTES de fechar o socket (eles ainda podem estar em send).
    {
        const std::scoped_lock lock{workers_mu};
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers.clear();
    }
    (void)peer.close();
    if (reader.joinable()) {
        reader.join();
    }
    return session_status;
}

Result<void> Server::serve_one() {
    auto peer = listener_.accept();
    if (!peer) {
        return std::unexpected(peer.error());
    }
    return handle_connection(std::move(*peer));
}

void Server::request_stop() noexcept {
    stop_requested_.store(true);
    static_cast<void>(listener_.close());
}

Result<void> Server::serve_forever() {
    while (!stop_requested_.load()) {
        auto status = serve_one();
        if (!status) {
            if (stop_requested_.load()) {
                return {};
            }
            // Listener fechado externamente conta como parada limpa.
            if (status.error().code == ErrorCode::connection_closed ||
                status.error().code == ErrorCode::io_error) {
                if (!listener_.is_open()) {
                    stop_requested_.store(true);
                    return {};
                }
            }
            return status;
        }
    }
    return {};
}

} // namespace modb::net
