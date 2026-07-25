// Cobre como o Client (Fase 8) reage a um handshake hostil — nenhum teste
// anterior exercitava isso porque todo teste de rede existente conversa com
// um Server real e bem-comportado do outro lado. Um "servidor" de mentira
// aqui aceita UMA conexão, descarta a Hello recebida e responde com a
// mensagem fornecida, para forçar exatamente o byte que se quer testar.
#include "modb/net/client.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/net/protocol.hpp"
#include "modb/net/server.hpp"

#include "test_support.hpp"

#include <thread>
#include <utility>

using namespace modb;
using namespace modb::net;

namespace {

Result<Client> connect_against_hostile_hello(const Message& canned_reply) {
    auto listener = NativeSocket::listen("127.0.0.1", 0);
    if (!listener) {
        return std::unexpected(listener.error());
    }
    auto port = listener->local_port();
    if (!port) {
        return std::unexpected(port.error());
    }
    std::thread acceptor([listener = std::move(*listener), &canned_reply]() mutable {
        auto accepted = listener.accept();
        if (!accepted) {
            return;
        }
        auto hello = recv_message(*accepted);
        (void)hello;
        (void)send_message(*accepted, canned_reply);
    });
    auto client = Client::connect("127.0.0.1", *port, "irrelevant");
    acceptor.join();
    return client;
}

} // namespace

int main() {
    TestSuite suite;

    // Ninguém escutando na porta: connect() propaga o erro do socket. O
    // listener descartável reserva uma porta livre e é fechado (fim do
    // escopo) ANTES do connect(), então ninguém mais escuta nela.
    {
        Result<std::uint16_t> port{std::unexpected(Error{ErrorCode::io_error, "unset"})};
        {
            auto listener = NativeSocket::listen("127.0.0.1", 0);
            suite.check(listener.has_value(), "a throwaway listener is created to reserve a port");
            if (listener) {
                port = listener->local_port();
                suite.check(port.has_value(), "the throwaway listener's port is read");
            }
        }  // o listener fecha aqui
        if (port) {
            suite.check_error(Client::connect("127.0.0.1", *port, "db"), ErrorCode::io_error,
                              "connecting to a closed port propagates the socket error");
        }
    }

    // O servidor responde com uma mensagem que NÃO é HelloOk.
    {
        auto client = connect_against_hostile_hello(Cancel{.query_id = 0});
        suite.check_error(client, ErrorCode::protocol_error,
                          "a non-HelloOk first reply is rejected");
    }

    // HelloOk com minor maior que o do cliente.
    {
        HelloOk reply;
        reply.minor = static_cast<std::uint16_t>(protocol_minor + 1);
        auto client = connect_against_hostile_hello(reply);
        suite.check_error(client, ErrorCode::incompatible_protocol_version,
                          "a server-selected minor above the client's is rejected");
    }

    // HelloOk com max_expansion_ratio zerado.
    {
        HelloOk reply;
        reply.max_expansion_ratio = 0;
        auto client = connect_against_hostile_hello(reply);
        suite.check_error(client, ErrorCode::protocol_error,
                          "a zero max_expansion_ratio from the server is rejected");
    }

    return suite.finish();
}
