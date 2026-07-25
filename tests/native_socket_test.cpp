// Cobre operações num NativeSocket fechado (default-construído, nunca
// conectado/escutando), o operador de move-assignment e a resolução de
// hostname via getaddrinfo — nenhum teste dedicado existia para este
// arquivo; a cobertura vinha só de forma indireta pelos testes de servidor,
// que sempre usam sockets já conectados/escutando e sempre "127.0.0.1"
// literal (que resolve_ipv4 resolve via inet_pton, sem nunca cair no
// fallback de getaddrinfo).
#include "modb/net/native_socket.hpp"

#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <utility>

using namespace modb;
using namespace modb::net;

int main() {
    TestSuite suite;

    // --- operações num socket fechado (default-construído) ---
    {
        NativeSocket closed;
        suite.check(!closed.is_open(), "a default-constructed socket is not open");

        suite.check_error(closed.accept(), ErrorCode::invalid_argument,
                          "accept() on a closed socket is rejected");

        std::array<std::byte, 4> buffer{};
        suite.check_error(closed.send_all(buffer), ErrorCode::invalid_argument,
                          "send_all() on a closed socket is rejected");
        suite.check_error(closed.recv_exact(buffer), ErrorCode::invalid_argument,
                          "recv_exact() on a closed socket is rejected");
        suite.check_error(closed.local_port(), ErrorCode::invalid_argument,
                          "local_port() on a closed socket is rejected");
        suite.check_error(closed.set_send_buffer_bytes(4096), ErrorCode::invalid_argument,
                          "set_send_buffer_bytes() on a closed socket is rejected");
        suite.check_error(closed.set_recv_buffer_bytes(4096), ErrorCode::invalid_argument,
                          "set_recv_buffer_bytes() on a closed socket is rejected");
        suite.check_error(closed.set_recv_timeout_ms(100), ErrorCode::invalid_argument,
                          "set_recv_timeout_ms() on a closed socket is rejected");
    }

    // --- move-assignment: o destino assume o socket de origem e fecha o seu ---
    {
        auto first = NativeSocket::listen("127.0.0.1", 0);
        auto second = NativeSocket::listen("127.0.0.1", 0);
        suite.check(first.has_value() && second.has_value(),
                    "two throwaway listeners are created for move-assignment");
        if (first && second) {
            const auto second_port = second->local_port();
            suite.check(second_port.has_value(), "the second listener's port is read");

            *first = std::move(*second);
            suite.check(first->is_open(), "move-assignment leaves the destination open");
            suite.check(first->local_port() == second_port,
                        "move-assignment transfers the source socket's identity");
        }
    }

    // --- resolução por hostname: "localhost" não é um literal dotted-decimal,
    // então força o fallback de getaddrinfo em resolve_ipv4 (127.0.0.1 nunca
    // exercita esse caminho, porque inet_pton resolve o literal direto). ---
    {
        auto listener = NativeSocket::listen("localhost", 0);
        suite.check(listener.has_value(), "listening on the \"localhost\" hostname resolves");
        if (listener) {
            auto port = listener->local_port();
            suite.check(port.has_value(), "the localhost listener's port is read");
            if (port) {
                auto connected = NativeSocket::connect("localhost", *port);
                suite.check(connected.has_value(),
                            "connecting to the \"localhost\" hostname resolves");
            }
        }
    }

    return suite.finish();
}
