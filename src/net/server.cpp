#include "modb/net/server.hpp"

#include "modb/object/object_codec.hpp"
#include "modb/storage/endian.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace modb::net {
namespace {

Error make_protocol(std::string message) {
    return Error{ErrorCode::protocol_error, std::move(message)};
}

constexpr std::size_t k_small_socket_buffer = 4 * 1024;

} // namespace

Result<void> send_message(NativeSocket& socket, const Message& message) {
    auto encoded = encode_message(message);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return socket.send_all(*encoded);
}

Result<Message> recv_message(NativeSocket& socket) {
    std::array<std::byte, 4> length_bytes{};
    if (auto status = socket.recv_exact(length_bytes); !status) {
        return std::unexpected(status.error());
    }
    const auto length =
        storage::load_le<std::uint32_t>(std::span<const std::byte>{length_bytes});
    if (length == 0) {
        return std::unexpected(make_protocol("frame length is zero"));
    }
    if (length > max_frame_bytes) {
        return std::unexpected(
            Error{ErrorCode::frame_too_large, "frame length exceeds 16 MiB"});
    }

    std::vector<std::byte> frame(4u + length);
    std::copy(length_bytes.begin(), length_bytes.end(), frame.begin());
    if (auto status = socket.recv_exact(std::span<std::byte>{frame.data() + 4, length}); !status) {
        return std::unexpected(status.error());
    }
    return decode_message(frame);
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
      last_stats_{other.last_stats_} {
    other.database_id_ = object::DatabaseId{};
    other.fail_after_.reset();
    other.small_buffers_ = false;
    other.last_stats_ = {};
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

Result<void> Server::handle_query(NativeSocket& peer, const Query& query) {
    last_stats_ = {};

    if (auto status = send_message(peer, StreamBegin{.query_id = query.query_id}); !status) {
        return status;
    }

    object::Database::ObjectQuerySpec spec{
        .type = query.description.type,
        .limit = query.description.limit,
        .project = query.description.project,
    };
    if (query.description.equals) {
        spec.equals = std::pair{query.description.equals->field, query.description.equals->value};
    }

    // Fila de saída: no máximo um frame com ≤ max_in_flight_objects. Enquanto
    // send_all bloqueia (cliente lento / SO_SNDBUF cheio), o generator não
    // avança — backpressure natural até o scan.
    ObjectFrame batch{.query_id = query.query_id, .compression = Compression::none};

    auto note_outstanding = [&] {
        const auto outstanding = last_stats_.produced - last_stats_.sent;
        if (outstanding > last_stats_.max_outstanding) {
            last_stats_.max_outstanding = outstanding;
        }
    };

    auto flush_batch = [&]() -> Result<void> {
        if (batch.records.empty()) {
            return {};
        }
        const auto count = batch.records.size();
        if (auto status = send_message(peer, batch); !status) {
            // Desconexão no meio do fluxo: abandona o generator ao sair do
            // laço — o Snapshot RAII libera o cursor (Fase 8D).
            return status;
        }
        last_stats_.sent += count;
        note_outstanding();
        batch.records.clear();
        return {};
    };

    for (auto& item : database_->query_objects(std::move(spec))) {
        if (!item) {
            (void)flush_batch();
            return send_message(peer, StreamError{.query_id = query.query_id,
                                                  .code = item.error().code,
                                                  .message = item.error().message});
        }

        if (fail_after_ && last_stats_.produced >= *fail_after_) {
            (void)flush_batch();
            return send_message(peer,
                                StreamError{.query_id = query.query_id,
                                            .code = ErrorCode::io_error,
                                            .message = "injected stream failure"});
        }

        auto payload = object::encode_object_payload(item->fields);
        if (!payload) {
            (void)flush_batch();
            return send_message(peer, StreamError{.query_id = query.query_id,
                                                  .code = payload.error().code,
                                                  .message = payload.error().message});
        }

        batch.records.push_back(ObjectEnvelope{
            .object_id = item->id,
            .type_definition_id = item->type,
            .payload = std::move(*payload),
        });
        ++last_stats_.produced;
        note_outstanding();

        if (batch.records.size() >= max_in_flight_objects) {
            if (auto status = flush_batch(); !status) {
                return status;
            }
        }
    }

    if (auto status = flush_batch(); !status) {
        return status;
    }
    return send_message(peer,
                        StreamEnd{.query_id = query.query_id, .total = last_stats_.produced});
}

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
    if (hello->version != protocol_version) {
        return std::unexpected(make_protocol("unsupported protocol version"));
    }
    const bool accepts_none =
        std::find(hello->accepted_codecs.begin(), hello->accepted_codecs.end(),
                  Compression::none) != hello->accepted_codecs.end();
    if (!accepts_none) {
        return std::unexpected(make_protocol("client must accept compression=none"));
    }
    (void)database_name_;

    HelloOk ok{.version = protocol_version,
               .baseline = baseline_,
               .selected_codec = Compression::none,
               .max_frame_bytes = max_frame_bytes};
    if (auto status = send_message(peer, ok); !status) {
        return status;
    }

    // Após o handshake, uma Query opcional. Desconexão limpa sem Query também
    // é válida (comportamento 8B / ping).
    auto next = recv_message(peer);
    if (!next) {
        if (next.error().code == ErrorCode::connection_closed) {
            return peer.close();
        }
        return std::unexpected(next.error());
    }
    const auto* query = std::get_if<Query>(&*next);
    if (query == nullptr) {
        return std::unexpected(make_protocol("expected Query after HelloOk"));
    }
    if (auto status = handle_query(peer, *query); !status) {
        // Erro de envio/desconexão: snapshot já liberado ao destruir o
        // generator; propaga o status para o chamador.
        return status;
    }
    return peer.close();
}

Result<void> Server::serve_one() {
    auto peer = listener_.accept();
    if (!peer) {
        return std::unexpected(peer.error());
    }
    return handle_connection(std::move(*peer));
}

} // namespace modb::net
