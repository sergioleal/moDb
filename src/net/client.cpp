#include "modb/net/client.hpp"

#include "modb/net/server.hpp"
#include "modb/object/object_codec.hpp"

#include <utility>
#include <variant>

namespace modb::net {
namespace {

Error make_protocol(std::string message) {
    return Error{ErrorCode::protocol_error, std::move(message)};
}

[[nodiscard]] std::optional<std::uint32_t> message_query_id(const Message& message) {
    if (const auto* begin = std::get_if<StreamBegin>(&message)) {
        return begin->query_id;
    }
    if (const auto* frame = std::get_if<ObjectFrame>(&message)) {
        return frame->query_id;
    }
    if (const auto* end = std::get_if<StreamEnd>(&message)) {
        return end->query_id;
    }
    if (const auto* error = std::get_if<StreamError>(&message)) {
        return error->query_id;
    }
    if (const auto* result = std::get_if<OpResult>(&message)) {
        return result->call_id;
    }
    return std::nullopt;
}

} // namespace

ClientConn::ClientConn(NativeSocket sock) : socket{std::move(sock)} {
    reader = std::thread([this] { reader_loop(); });
}

ClientConn::~ClientConn() {
    stop.store(true, std::memory_order_relaxed);
    (void)socket.close();
    if (reader.joinable()) {
        reader.join();
    }
}

void ClientConn::reader_loop() {
    while (!stop.load(std::memory_order_relaxed)) {
        auto message = recv_message(socket);
        if (!message) {
            failed.store(true, std::memory_order_relaxed);
            failure = message.error();
            stop.store(true, std::memory_order_relaxed);
            cv.notify_all();
            return;
        }
        const auto query_id = message_query_id(*message);
        if (!query_id) {
            failed.store(true, std::memory_order_relaxed);
            failure = make_protocol("server sent message without query_id");
            stop.store(true, std::memory_order_relaxed);
            cv.notify_all();
            return;
        }
        {
            const std::scoped_lock lock{mu};
            mailboxes[*query_id].push_back(std::move(*message));
        }
        cv.notify_all();
    }
}

Result<Message> ClientConn::recv_for(std::uint32_t query_id) {
    std::unique_lock lock{mu};
    cv.wait(lock, [&] {
        return stop.load(std::memory_order_relaxed) || !mailboxes[query_id].empty();
    });
    if (!mailboxes[query_id].empty()) {
        Message message = std::move(mailboxes[query_id].front());
        mailboxes[query_id].pop_front();
        return message;
    }
    if (failed.load(std::memory_order_relaxed)) {
        return std::unexpected(failure);
    }
    return std::unexpected(Error{ErrorCode::connection_closed, "client connection stopped"});
}

Result<void> ClientConn::send(const Message& message) {
    const std::scoped_lock lock{send_mu};
    return send_message(socket, message);
}

void NextAwaitable::await_suspend(std::coroutine_handle<> handle) {
    // Executor = thread do chamador: next() bloqueia no mailbox/socket e em
    // seguida retoma a coroutine (ADR-011).
    result_ = stream->next();
    handle.resume();
}

ObjectStream::ObjectStream(std::shared_ptr<ClientConn> conn, std::uint32_t query_id)
    : conn_{std::move(conn)}, query_id_{query_id} {}

ObjectStream::ObjectStream(ObjectStream&& other) noexcept
    : conn_{std::move(other.conn_)}, query_id_{other.query_id_}, pending_{std::move(other.pending_)},
      pending_index_{other.pending_index_}, begun_{other.begun_}, finished_{other.finished_},
      received_{other.received_} {
    other.finished_ = true;
}

ObjectStream::~ObjectStream() = default;

Result<void> ObjectStream::refill() {
    if (!conn_) {
        return std::unexpected(make_protocol("object stream has no connection"));
    }
    auto message = conn_->recv_for(query_id_);
    if (!message) {
        return std::unexpected(message.error());
    }

    if (const auto* begin = std::get_if<StreamBegin>(&*message)) {
        if (begun_) {
            return std::unexpected(make_protocol("duplicate StreamBegin"));
        }
        if (begin->query_id != query_id_) {
            return std::unexpected(make_protocol("StreamBegin query_id mismatch"));
        }
        begun_ = true;
        return refill();
    }

    if (const auto* frame = std::get_if<ObjectFrame>(&*message)) {
        if (!begun_) {
            return std::unexpected(make_protocol("ObjectFrame before StreamBegin"));
        }
        if (frame->query_id != query_id_) {
            return std::unexpected(make_protocol("ObjectFrame query_id mismatch"));
        }
        if (frame->compression != Compression::none &&
            frame->compression != conn_->selected_codec) {
            return std::unexpected(make_protocol("ObjectFrame compression was not negotiated"));
        }
        pending_.clear();
        pending_index_ = 0;
        pending_.reserve(frame->records.size());
        for (const auto& envelope : frame->records) {
            auto fields = object::decode_object_payload(envelope.payload);
            if (!fields) {
                return std::unexpected(fields.error());
            }
            pending_.push_back(object::DecodedObject{
                .id = envelope.object_id,
                .type = envelope.type_definition_id,
                .fields = std::move(*fields),
            });
        }
        return {};
    }

    if (const auto* end = std::get_if<StreamEnd>(&*message)) {
        if (!begun_) {
            return std::unexpected(make_protocol("StreamEnd before StreamBegin"));
        }
        if (end->query_id != query_id_) {
            return std::unexpected(make_protocol("StreamEnd query_id mismatch"));
        }
        const auto remaining = pending_.size() - pending_index_;
        if (end->total != received_ + remaining) {
            return std::unexpected(make_protocol("StreamEnd total mismatch"));
        }
        finished_ = true;
        return {};
    }

    if (const auto* error = std::get_if<StreamError>(&*message)) {
        if (error->query_id != query_id_) {
            return std::unexpected(make_protocol("StreamError query_id mismatch"));
        }
        finished_ = true;
        return std::unexpected(Error{error->code, error->message});
    }

    return std::unexpected(make_protocol("unexpected message in object stream"));
}

Result<std::optional<object::DecodedObject>> ObjectStream::next() {
    if (finished_ && pending_index_ >= pending_.size()) {
        return std::optional<object::DecodedObject>{};
    }
    while (pending_index_ >= pending_.size()) {
        if (finished_) {
            return std::optional<object::DecodedObject>{};
        }
        if (auto status = refill(); !status) {
            return std::unexpected(status.error());
        }
        if (finished_ && pending_index_ >= pending_.size()) {
            return std::optional<object::DecodedObject>{};
        }
    }
    object::DecodedObject object = std::move(pending_[pending_index_++]);
    ++received_;
    return object;
}

Client::Client(std::shared_ptr<ClientConn> conn, HelloOk hello_ok)
    : conn_{std::move(conn)}, hello_ok_{std::move(hello_ok)} {}

Client::Client(Client&& other) noexcept
    : conn_{std::move(other.conn_)}, hello_ok_{std::move(other.hello_ok_)},
      next_query_id_{other.next_query_id_} {}

Client::~Client() = default;

Result<Client> Client::connect(std::string_view host, std::uint16_t port,
                               std::string_view database_name) {
    auto socket = NativeSocket::connect(host, port);
    if (!socket) {
        return std::unexpected(socket.error());
    }

    Hello hello{.version = protocol_version,
                .database_name = std::string{database_name},
                .accepted_codecs = {Compression::none, Compression::rle}};
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
    if (!is_known_compression(ok->selected_codec)) {
        return std::unexpected(make_protocol("server selected unknown compression codec"));
    }
    if (ok->max_expansion_ratio == 0) {
        return std::unexpected(make_protocol("server sent invalid max_expansion_ratio"));
    }

    auto conn = std::make_shared<ClientConn>(std::move(*socket));
    conn->selected_codec = ok->selected_codec;
    conn->max_expansion_ratio = ok->max_expansion_ratio;
    return Client{std::move(conn), *ok};
}

Result<void> Client::set_recv_buffer_bytes(std::size_t bytes) {
    if (!conn_) {
        return std::unexpected(Error{ErrorCode::connection_closed, "client has no connection"});
    }
    return conn_->socket.set_recv_buffer_bytes(bytes);
}

Result<ObjectStream> Client::query(QueryDescription description) {
    if (!conn_) {
        return std::unexpected(Error{ErrorCode::connection_closed, "client socket is closed"});
    }
    const auto query_id = next_query_id_++;
    Query message{.query_id = query_id, .description = std::move(description)};
    if (auto status = conn_->send(message); !status) {
        return std::unexpected(status.error());
    }
    return ObjectStream{conn_, query_id};
}

Result<void> Client::cancel(std::uint32_t query_id) {
    if (!conn_) {
        return std::unexpected(Error{ErrorCode::connection_closed, "client socket is closed"});
    }
    return conn_->send(Cancel{.query_id = query_id});
}

Result<std::vector<std::byte>> Client::call(std::string_view operation_id,
                                            std::span<const std::byte> args) {
    if (!conn_) {
        return std::unexpected(Error{ErrorCode::connection_closed, "client socket is closed"});
    }
    const auto call_id = next_query_id_++;
    OpCall message{.call_id = call_id,
                   .operation_id = std::string{operation_id},
                   .args = {args.begin(), args.end()}};
    if (auto status = conn_->send(message); !status) {
        return std::unexpected(status.error());
    }
    auto reply = conn_->recv_for(call_id);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    const auto* result = std::get_if<OpResult>(&*reply);
    if (result == nullptr) {
        return std::unexpected(make_protocol("expected OpResult from server"));
    }
    if (result->call_id != call_id) {
        return std::unexpected(make_protocol("OpResult call_id mismatch"));
    }
    if (!result->ok) {
        return std::unexpected(Error{result->code, result->message});
    }
    return result->payload;
}

Result<HelloOk> Client::handshake(std::string_view host, std::uint16_t port,
                                  std::string_view database_name) {
    auto client = connect(host, port, database_name);
    if (!client) {
        return std::unexpected(client.error());
    }
    return client->hello_ok();
}

} // namespace modb::net
