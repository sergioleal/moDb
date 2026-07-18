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

} // namespace

ObjectStream::ObjectStream(NativeSocket* socket, std::uint32_t query_id)
    : socket_{socket}, query_id_{query_id} {}

ObjectStream::ObjectStream(ObjectStream&& other) noexcept
    : socket_{other.socket_}, query_id_{other.query_id_}, pending_{std::move(other.pending_)},
      pending_index_{other.pending_index_}, begun_{other.begun_}, finished_{other.finished_},
      received_{other.received_} {
    other.socket_ = nullptr;
    other.finished_ = true;
}

ObjectStream::~ObjectStream() = default;

Result<void> ObjectStream::refill() {
    if (socket_ == nullptr) {
        return std::unexpected(make_protocol("object stream has no socket"));
    }
    auto message = recv_message(*socket_);
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
        if (frame->compression != Compression::none) {
            return std::unexpected(make_protocol("Fase 8C only accepts compression=none"));
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
        if (end->total != received_ + pending_.size() - pending_index_) {
            // total conta objetos já entregues ao cliente + ainda no pending.
            // Ajuste: received_ ainda não inclui pending; total do servidor é
            // o total emitido. Aceitamos total == received_ + remaining pending
            // no momento do End (pending já carregado).
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

Client::Client(NativeSocket socket, HelloOk hello_ok)
    : socket_{std::move(socket)}, hello_ok_{std::move(hello_ok)} {}

Client::Client(Client&& other) noexcept
    : socket_{std::move(other.socket_)}, hello_ok_{std::move(other.hello_ok_)},
      next_query_id_{other.next_query_id_}, stream_active_{other.stream_active_} {
    other.stream_active_ = false;
}

Client::~Client() = default;

Result<Client> Client::connect(std::string_view host, std::uint16_t port,
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
        return std::unexpected(make_protocol("Fase 8C only accepts compression=none"));
    }
    return Client{std::move(*socket), *ok};
}

Result<ObjectStream> Client::query(QueryDescription description) {
    if (!socket_.is_open()) {
        return std::unexpected(Error{ErrorCode::connection_closed, "client socket is closed"});
    }
    if (stream_active_) {
        return std::unexpected(
            make_protocol("another object stream is still active on this connection"));
    }
    const auto query_id = next_query_id_++;
    Query message{.query_id = query_id, .description = std::move(description)};
    if (auto status = send_message(socket_, message); !status) {
        return std::unexpected(status.error());
    }
    stream_active_ = true;
    return ObjectStream{&socket_, query_id};
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
