#pragma once

// Cliente TCP da Fase 8C: mantém a conexão após Hello/HelloOk e consome
// ObjectStream incremental (Query → Begin → Frame(s) → End/Error).

#include "modb/error.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/net/protocol.hpp"
#include "modb/object/object_codec.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modb::net {

class ObjectStream {
public:
    ObjectStream(const ObjectStream&) = delete;
    ObjectStream& operator=(const ObjectStream&) = delete;
    ObjectStream(ObjectStream&&) noexcept;
    ObjectStream& operator=(ObjectStream&&) = delete;
    ~ObjectStream();

    // Próximo objeto lógico; nullopt = StreamEnd. StreamError → unexpected.
    [[nodiscard]] Result<std::optional<object::DecodedObject>> next();

    [[nodiscard]] std::uint32_t query_id() const noexcept { return query_id_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] std::uint64_t received() const noexcept { return received_; }

private:
    friend class Client;
    ObjectStream(NativeSocket* socket, std::uint32_t query_id);

    [[nodiscard]] Result<void> refill();

    NativeSocket* socket_{nullptr};
    std::uint32_t query_id_{0};
    std::vector<object::DecodedObject> pending_{};
    std::size_t pending_index_{0};
    bool begun_{false};
    bool finished_{false};
    std::uint64_t received_{0};
};

class Client {
public:
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept;
    Client& operator=(Client&&) = delete;
    ~Client();

    // Conecta, completa Hello/HelloOk e mantém o socket aberto.
    [[nodiscard]] static Result<Client> connect(std::string_view host, std::uint16_t port,
                                                std::string_view database_name);

    [[nodiscard]] const HelloOk& hello_ok() const noexcept { return hello_ok_; }

    // Envia Query e devolve um stream sobre a mesma conexão.
    [[nodiscard]] Result<ObjectStream> query(QueryDescription description);

    // Conveniência 8B: handshake e fecha (sem streaming).
    [[nodiscard]] static Result<HelloOk> handshake(std::string_view host, std::uint16_t port,
                                                   std::string_view database_name);

private:
    Client(NativeSocket socket, HelloOk hello_ok);

    NativeSocket socket_;
    HelloOk hello_ok_{};
    std::uint32_t next_query_id_{1};
    bool stream_active_{false};
};

} // namespace modb::net
