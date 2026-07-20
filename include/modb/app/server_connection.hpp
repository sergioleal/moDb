#pragma once

// Camada de conveniencia para aplicacoes que consomem um servidor Ring0.
// Mantem o protocolo em modb::net, mas oferece um ponto de entrada estavel para
// apps: conectar, consultar, chamar operacoes e abrir facades remotas.

#include "modb/error.hpp"
#include "modb/net/client.hpp"
#include "modb/net/protocol.hpp"
#include "modb/net/query_description.hpp"
#include "modb/object/object_codec.hpp"
#include "modb/ops/facade_descriptor.hpp"
#include "modb/ops/facade_handle.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace modb::app {

struct ConnectionOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    std::string database_name{};
    std::optional<std::size_t> recv_buffer_bytes{};
};

struct ServerInfo {
    std::uint16_t protocol_major{net::protocol_major};
    std::uint16_t protocol_minor{net::protocol_minor};
    object::BaselineId baseline{};
    net::Compression selected_codec{net::Compression::none};
    std::uint32_t max_frame_bytes{net::max_frame_bytes};
    std::uint16_t max_concurrent_streams{net::default_max_concurrent_streams};
    std::uint32_t idle_timeout_ms{net::default_idle_timeout_ms};
};

class ServerConnection {
public:
    ServerConnection(const ServerConnection&) = delete;
    ServerConnection& operator=(const ServerConnection&) = delete;
    ServerConnection(ServerConnection&&) noexcept;
    ServerConnection& operator=(ServerConnection&&) = delete;
    ~ServerConnection();

    [[nodiscard]] static Result<ServerConnection> connect(ConnectionOptions options);
    [[nodiscard]] static Result<ServerInfo> handshake(const ConnectionOptions& options);

    [[nodiscard]] const ServerInfo& info() const noexcept { return info_; }

    [[nodiscard]] Result<net::ObjectStream> query(net::QueryDescription description);
    [[nodiscard]] Result<void> cancel(std::uint32_t query_id);
    [[nodiscard]] Result<std::vector<object::DecodedObject>>
    collect(net::QueryDescription description);

    [[nodiscard]] Result<std::vector<std::byte>> call(std::string_view operation_id,
                                                      std::span<const std::byte> args);
    [[nodiscard]] Result<std::vector<ops::FacadeDescriptor>> list_facades();
    [[nodiscard]] Result<net::FacadeOpenOk> open_facade(std::string_view facade_id,
                                                        std::uint32_t version);

    template <typename TFacade>
    [[nodiscard]] Result<ops::FacadeHandle<TFacade>> open_facade() {
        return client_.open_facade<TFacade>();
    }

private:
    ServerConnection(net::Client client, ServerInfo info);

    net::Client client_;
    ServerInfo info_{};
};

[[nodiscard]] ServerInfo server_info_from_hello(const net::HelloOk& hello) noexcept;

} // namespace modb::app
