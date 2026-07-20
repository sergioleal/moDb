#include "modb/app/server_connection.hpp"

#include <utility>

namespace modb::app {

ServerInfo server_info_from_hello(const net::HelloOk& hello) noexcept {
    return ServerInfo{
        .protocol_major = hello.version,
        .protocol_minor = hello.minor,
        .baseline = hello.baseline,
        .selected_codec = hello.selected_codec,
        .max_frame_bytes = hello.max_frame_bytes,
        .max_concurrent_streams = hello.max_concurrent_streams,
        .idle_timeout_ms = hello.idle_timeout_ms,
    };
}

ServerConnection::ServerConnection(net::Client client, ServerInfo info)
    : client_{std::move(client)}, info_{info} {}

ServerConnection::ServerConnection(ServerConnection&&) noexcept = default;
ServerConnection::~ServerConnection() = default;

Result<ServerConnection> ServerConnection::connect(ConnectionOptions options) {
    if (options.port == 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "server port must be non-zero"});
    }
    if (options.database_name.empty()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "database_name must not be empty"});
    }

    auto client = net::Client::connect(options.host, options.port, options.database_name);
    if (!client) {
        return std::unexpected(client.error());
    }
    if (options.recv_buffer_bytes) {
        if (auto status = client->set_recv_buffer_bytes(*options.recv_buffer_bytes); !status) {
            return std::unexpected(status.error());
        }
    }

    const auto info = server_info_from_hello(client->hello_ok());
    return ServerConnection{std::move(*client), info};
}

Result<ServerInfo> ServerConnection::handshake(const ConnectionOptions& options) {
    if (options.port == 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "server port must be non-zero"});
    }
    if (options.database_name.empty()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "database_name must not be empty"});
    }
    auto hello = net::Client::handshake(options.host, options.port, options.database_name);
    if (!hello) {
        return std::unexpected(hello.error());
    }
    return server_info_from_hello(*hello);
}

Result<net::ObjectStream> ServerConnection::query(net::QueryDescription description) {
    return client_.query(std::move(description));
}

Result<void> ServerConnection::cancel(std::uint32_t query_id) { return client_.cancel(query_id); }

Result<std::vector<object::DecodedObject>>
ServerConnection::collect(net::QueryDescription description) {
    auto stream = query(std::move(description));
    if (!stream) {
        return std::unexpected(stream.error());
    }

    std::vector<object::DecodedObject> objects;
    while (true) {
        auto next = stream->next();
        if (!next) {
            return std::unexpected(next.error());
        }
        if (!*next) {
            break;
        }
        objects.push_back(std::move(**next));
    }
    return objects;
}

Result<std::vector<std::byte>> ServerConnection::call(std::string_view operation_id,
                                                      std::span<const std::byte> args) {
    return client_.call(operation_id, args);
}

Result<std::vector<ops::FacadeDescriptor>> ServerConnection::list_facades() {
    return client_.list_facades();
}

Result<net::FacadeOpenOk> ServerConnection::open_facade(std::string_view facade_id,
                                                        std::uint32_t version) {
    return client_.open_facade(facade_id, version);
}

} // namespace modb::app
