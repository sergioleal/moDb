#pragma once

// Cliente TCP das Fases 8C–8E: demux por query_id, Cancel, multiplexação e
// co_await await_next() (executor = thread do chamador, ADR-011).

#include "modb/error.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/net/protocol.hpp"
#include "modb/object/object_codec.hpp"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace modb::net {

class Client;

// Estado compartilhado da conexão: leitor em background demultiplexa frames.
struct ClientConn {
    NativeSocket socket;
    Compression selected_codec{Compression::none};
    std::uint16_t max_expansion_ratio{default_max_expansion_ratio};
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<std::uint32_t, std::deque<Message>> mailboxes;
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    Error failure{ErrorCode::connection_closed, "connection closed"};
    std::thread reader;
    std::mutex send_mu;

    explicit ClientConn(NativeSocket sock);
    ~ClientConn();

    ClientConn(const ClientConn&) = delete;
    ClientConn& operator=(const ClientConn&) = delete;

    void reader_loop();
    [[nodiscard]] Result<Message> recv_for(std::uint32_t query_id);
    [[nodiscard]] Result<void> send(const Message& message);
};

// Awaitable de next() — executor = thread do chamador (espera bloqueante no
// socket/mailbox). Uso: `auto item = co_await stream.await_next();`
struct NextAwaitable {
    class ObjectStream* stream{nullptr};
    Result<std::optional<object::DecodedObject>> result_{
        std::unexpected(Error{ErrorCode::invalid_argument, "awaitable not started"})};

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle);
    Result<std::optional<object::DecodedObject>> await_resume() { return std::move(result_); }
};

class ObjectStream {
public:
    ObjectStream(const ObjectStream&) = delete;
    ObjectStream& operator=(const ObjectStream&) = delete;
    ObjectStream(ObjectStream&&) noexcept;
    ObjectStream& operator=(ObjectStream&&) = delete;
    ~ObjectStream();

    // Próximo objeto lógico; nullopt = StreamEnd. StreamError → unexpected.
    [[nodiscard]] Result<std::optional<object::DecodedObject>> next();

    // Fase 8E: mesma semântica de next(), usável com co_await.
    [[nodiscard]] NextAwaitable await_next() { return NextAwaitable{this}; }

    [[nodiscard]] std::uint32_t query_id() const noexcept { return query_id_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] std::uint64_t received() const noexcept { return received_; }

private:
    friend class Client;
    friend struct NextAwaitable;
    ObjectStream(std::shared_ptr<ClientConn> conn, std::uint32_t query_id);

    [[nodiscard]] Result<void> refill();

    std::shared_ptr<ClientConn> conn_;
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

    [[nodiscard]] static Result<Client> connect(std::string_view host, std::uint16_t port,
                                                std::string_view database_name);

    [[nodiscard]] const HelloOk& hello_ok() const noexcept { return hello_ok_; }

    [[nodiscard]] Result<void> set_recv_buffer_bytes(std::size_t bytes);

    // Envia Query; várias streams podem coexistir (demux por query_id).
    [[nodiscard]] Result<ObjectStream> query(QueryDescription description);

    // Envia Cancel para interromper a produção no servidor (Fase 8E).
    [[nodiscard]] Result<void> cancel(std::uint32_t query_id);

    // Fase 9: despacha operação de domínio (OpCall/OpResult).
    [[nodiscard]] Result<std::vector<std::byte>> call(std::string_view operation_id,
                                                      std::span<const std::byte> args);

    // Fase 11C: descoberta e negociação de facades.
    [[nodiscard]] Result<std::vector<ops::FacadeDescriptor>> list_facades();
    [[nodiscard]] Result<FacadeOpenOk> open_facade(std::string_view facade_id,
                                                   std::uint32_t version);

    [[nodiscard]] static Result<HelloOk> handshake(std::string_view host, std::uint16_t port,
                                                   std::string_view database_name);

private:
    Client(std::shared_ptr<ClientConn> conn, HelloOk hello_ok);

    std::shared_ptr<ClientConn> conn_;
    HelloOk hello_ok_{};
    std::uint32_t next_query_id_{1};
};

} // namespace modb::net
