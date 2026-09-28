#include "modb/net/server.hpp"

#include "engine_session.hpp"

#include "modb/compatibility.hpp"
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

// Transporte da sessão direta: uma thread leitora lê o socket e põe as
// mensagens numa fila; a thread da sessão as entrega à `EngineSession`.
struct DirectConnection {
    NativeSocket* peer{nullptr};
    EngineSession* engine{nullptr};
    std::mutex inbox_mu;
    std::condition_variable inbox_cv;
    std::deque<Message> inbox;
    std::atomic<bool> stop{false};
    std::atomic<bool> reader_failed{false};
    Error reader_error{ErrorCode::connection_closed, "reader stopped"};
};

void reader_loop(DirectConnection& session) {
    while (!session.stop.load(std::memory_order_relaxed)) {
        auto message = recv_message(*session.peer);
        if (!message) {
            // Mesma corrida do lado cliente (ver ClientConn::reader_loop):
            // `reader_error` contém um std::string que `wait_inbound` copia com
            // `inbox_mu` tomado, então publicá-lo sem o mutex libera o buffer
            // antigo debaixo do consumidor e corrompe o heap. Os flags entram no
            // mesmo escopo para só ficarem visíveis com o Error já completo.
            {
                const std::scoped_lock lock{session.inbox_mu};
                session.reader_error = message.error();
                session.reader_failed.store(true, std::memory_order_relaxed);
                session.stop.store(true, std::memory_order_relaxed);
            }
            session.inbox_cv.notify_all();
            return;
        }
        // Cancel não espera a fila: a thread da sessão pode estar num OpCall.
        if (session.engine->handle_urgent(*message)) {
            continue;
        }
        {
            const std::scoped_lock lock{session.inbox_mu};
            session.inbox.push_back(std::move(*message));
        }
        session.inbox_cv.notify_all();
    }
}

[[nodiscard]] Result<Message> wait_inbound(DirectConnection& session) {
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
      // A configuração também acompanha o servidor movido: sem isto, um Server
      // configurado e depois devolvido por valor perdia o registro de procs
      // (OpCall → "server has no operation registry") e os limites negociados.
      max_concurrent_streams_{other.max_concurrent_streams_}, idle_timeout_ms_{other.idle_timeout_ms_},
      preferred_codec_{other.preferred_codec_},
      selected_codec_{other.selected_codec_.load(std::memory_order_relaxed)},
      last_stats_{other.last_stream_stats()}, operations_{std::move(other.operations_)},
      facades_{std::move(other.facades_)},
      stop_requested_{other.stop_requested_.load(std::memory_order_relaxed)},
      active_{std::move(other.active_)} {
    other.database_id_ = object::DatabaseId{};
    other.fail_after_.reset();
    other.small_buffers_ = false;
    {
        const std::scoped_lock lock{*other.stats_mutex_};
        other.last_stats_ = {};
    }
    other.selected_codec_.store(Compression::none, std::memory_order_relaxed);
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

StreamStats Server::last_stream_stats() const noexcept {
    const std::scoped_lock lock{*stats_mutex_};
    return last_stats_;
}

Result<void> Server::handle_connection(NativeSocket& peer) {
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

    SocketSink sink{peer};
    EngineSession engine{session_services(), sink, selected};
    DirectConnection session;
    session.peer = &peer;
    session.engine = &engine;
    std::thread reader{[&session] { reader_loop(session); }};

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
        if (auto status = engine.handle(*inbound); !status) {
            session_status = std::unexpected(status.error());
            break;
        }
    }

    session.stop.store(true, std::memory_order_relaxed);
    // Cancela os streams e junta workers e anel ANTES de fechar o socket (os
    // workers ainda podem estar em send).
    engine.finish();
    // shutdown acorda a thread leitora; só depois de ela sair o socket fecha
    // (fechar com ela ainda num recv era a corrida em NativeSocket::close).
    (void)peer.shutdown();
    if (reader.joinable()) {
        reader.join();
    }
    (void)peer.close();
    return session_status;
}

EngineServices Server::session_services() {
    return EngineServices{
        .database = database_,
        .operations = operations_,
        .facades = facades_,
        .fail_after = fail_after_,
        .max_concurrent_streams = max_concurrent_streams_,
        .allow_shm = true,
        .on_stream_stats =
            [this](const StreamStats& stats) {
                const std::scoped_lock lock{*stats_mutex_};
                last_stats_ = stats;
            },
        .server_stop = &stop_requested_,
    };
}

Result<void> Server::serve_one() {
    auto peer = listener_.accept();
    if (!peer) {
        return std::unexpected(peer.error());
    }
    return handle_connection(*peer);
}

void Server::request_stop() noexcept {
    stop_requested_.store(true);
    static_cast<void>(listener_.close());
    // Desligamento ativo: sessões ociosas estão bloqueadas lendo o socket e só
    // perceberiam a parada no idle timeout; shutdown as acorda agora.
    if (active_) {
        const std::scoped_lock lock{active_->mu};
        for (auto& [id, socket] : active_->sockets) {
            static_cast<void>(socket->shutdown());
        }
    }
}

Result<void> Server::serve_forever() {
    std::mutex sessions_mu;
    std::vector<std::thread> sessions;
    Result<void> loop_status{};

    const auto join_sessions = [&] {
        const std::scoped_lock lock{sessions_mu};
        for (auto& session : sessions) {
            if (session.joinable()) {
                session.join();
            }
        }
        sessions.clear();
    };

    while (!stop_requested_.load()) {
        auto peer = listener_.accept();
        if (!peer) {
            if (stop_requested_.load()) {
                break;
            }
            // Listener fechado externamente conta como parada limpa.
            if (peer.error().code == ErrorCode::connection_closed ||
                peer.error().code == ErrorCode::io_error) {
                if (!listener_.is_open()) {
                    stop_requested_.store(true);
                    break;
                }
            }
            loop_status = std::unexpected(peer.error());
            break;
        }

        std::thread session_thread([this, peer = std::move(*peer)]() mutable {
            std::uint64_t id = 0;
            {
                const std::scoped_lock lock{active_->mu};
                id = ++active_->next_id;
                active_->sockets.emplace(id, &peer);
                // Parada pedida entre o accept e o registro: não espera o timeout.
                if (stop_requested_.load()) {
                    static_cast<void>(peer.shutdown());
                }
            }
            (void)handle_connection(peer);
            const std::scoped_lock lock{active_->mu};
            active_->sockets.erase(id);
        });
        {
            const std::scoped_lock lock{sessions_mu};
            sessions.push_back(std::move(session_thread));
        }
    }
    join_sessions();
    return loop_status;
}

} // namespace modb::net
