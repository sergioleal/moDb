#include "modb/net/server.hpp"

#include "modb/storage/binary.hpp"
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
      database_name_{std::move(other.database_name_)}, baseline_{other.baseline_} {
    other.database_id_ = object::DatabaseId{};
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
        // Banco novo: cria se não existir.
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

Result<void> Server::handle_connection(NativeSocket peer) {
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

    // O nome no Hello identifica o banco lógico; aceitamos o stem do arquivo ou
    // o nome pedido (CLI ping usa o stem).
    (void)database_name_;

    HelloOk ok{.version = protocol_version,
               .baseline = baseline_,
               .selected_codec = Compression::none,
               .max_frame_bytes = max_frame_bytes};
    if (auto status = send_message(peer, ok); !status) {
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

Result<HelloOk> Client::handshake(std::string_view host, std::uint16_t port,
                                  std::string_view database_name) {
    auto socket = NativeSocket::connect(host, port);
    if (!socket) {
        return std::unexpected(socket.error());
    }

    Hello hello{.version = protocol_version,
                .database_name = std::string{database_name},
                .accepted_codecs = {Compression::none}};
    if (auto status = send_message(*socket, hello); !status) {
        return std::unexpected(status.error());
    }

    auto reply = recv_message(*socket);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    const auto* ok = std::get_if<HelloOk>(&*reply);
    if (ok == nullptr) {
        return std::unexpected(make_protocol("expected HelloOk from server"));
    }
    if (ok->version != protocol_version) {
        return std::unexpected(make_protocol("server selected unsupported protocol version"));
    }
    if (ok->selected_codec != Compression::none) {
        return std::unexpected(make_protocol("Fase 8B only accepts compression=none"));
    }
    (void)socket->close();
    return *ok;
}

} // namespace modb::net
