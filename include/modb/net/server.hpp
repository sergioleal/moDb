#pragma once

// Servidor TCP das Fases 8B–8E: sessão com leitor ativo, Cancel, multiplexação
// de query_id (escrita serializada) e backpressure (fila ≤ max_in_flight).

#include "modb/error.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/net/protocol.hpp"
#include "modb/object/database.hpp"
#include "modb/query/operators.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace modb::net {

// Limite de objetos no frame em trânsito (fila de saída da Fase 8D).
inline constexpr std::size_t max_in_flight_objects = 8;

[[nodiscard]] Result<void> send_message(NativeSocket& socket, const Message& message);
[[nodiscard]] Result<Message> recv_message(NativeSocket& socket);

// Contadores do último fluxo (produzidos − enviados = objetos na fila local).
struct StreamStats {
    std::uint64_t produced{0};
    std::uint64_t sent{0};
    std::uint64_t max_outstanding{0};
};

class Server {
public:
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) noexcept;
    Server& operator=(Server&&) = delete;
    ~Server();

    [[nodiscard]] static Result<Server> listen(const std::filesystem::path& path,
                                               std::string_view host = "127.0.0.1",
                                               std::uint16_t port = 0);

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::string_view database_name() const noexcept { return database_name_; }
    [[nodiscard]] object::BaselineId baseline() const noexcept { return baseline_; }
    [[nodiscard]] const StreamStats& last_stream_stats() const noexcept { return last_stats_; }
    [[nodiscard]] std::size_t open_snapshot_count() const noexcept {
        return database_ ? database_->open_snapshot_count() : 0;
    }

    void fail_stream_after(std::size_t objects) noexcept { fail_after_ = objects; }
    void use_small_socket_buffers(bool enabled) noexcept { small_buffers_ = enabled; }

    // Aceita uma conexão e mantém a sessão até o peer fechar (Hello + Queries).
    [[nodiscard]] Result<void> serve_one();

private:
    Server(std::shared_ptr<object::Database> database, object::DatabaseId database_id,
           NativeSocket listener, std::uint16_t port, std::string database_name,
           object::BaselineId baseline);

    [[nodiscard]] Result<void> handle_connection(NativeSocket peer);

    std::shared_ptr<object::Database> database_;
    object::DatabaseId database_id_{};
    NativeSocket listener_;
    std::uint16_t port_{0};
    std::string database_name_;
    object::BaselineId baseline_{};
    std::optional<std::size_t> fail_after_{};
    bool small_buffers_{false};
    StreamStats last_stats_{};
};

} // namespace modb::net
