#pragma once

// Servidor TCP das Fases 8B–8C: Hello/HelloOk e execução de QueryDescription
// em streaming (Begin → ObjectFrame(s) → End/Error).

#include "modb/error.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/net/protocol.hpp"
#include "modb/object/database.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace modb::net {

[[nodiscard]] Result<void> send_message(NativeSocket& socket, const Message& message);
[[nodiscard]] Result<Message> recv_message(NativeSocket& socket);

class Server {
public:
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) noexcept;
    Server& operator=(Server&&) = delete;
    ~Server();

    // Abre o banco, anexa ao registry e faz bind em host:port (0 = efêmera).
    [[nodiscard]] static Result<Server> listen(const std::filesystem::path& path,
                                               std::string_view host = "127.0.0.1",
                                               std::uint16_t port = 0);

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::string_view database_name() const noexcept { return database_name_; }
    [[nodiscard]] object::BaselineId baseline() const noexcept { return baseline_; }

    // Uso em testes (8C): após emitir N objetos, envia StreamError.
    void fail_stream_after(std::size_t objects) noexcept { fail_after_ = objects; }

    // Aceita uma conexão: Hello/HelloOk, opcionalmente uma Query, e encerra.
    [[nodiscard]] Result<void> serve_one();

private:
    Server(std::shared_ptr<object::Database> database, object::DatabaseId database_id,
           NativeSocket listener, std::uint16_t port, std::string database_name,
           object::BaselineId baseline);

    [[nodiscard]] Result<void> handle_connection(NativeSocket peer);
    [[nodiscard]] Result<void> handle_query(NativeSocket& peer, const Query& query);

    std::shared_ptr<object::Database> database_;
    object::DatabaseId database_id_{};
    NativeSocket listener_;
    std::uint16_t port_{0};
    std::string database_name_;
    object::BaselineId baseline_{};
    std::optional<std::size_t> fail_after_{};
};

} // namespace modb::net
