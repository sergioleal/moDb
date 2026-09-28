// Socket local AF_UNIX (ADR-028, PLANO_PROXY X2): eco nos dois sentidos, frames
// do protocolo, socket velho no caminho, parada que acorda o accept e a
// leitura bloqueada, e caminho longo demais.

#include "modb/net/native_socket.hpp"
#include "modb/net/protocol.hpp"
#include "modb/net/server.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace modb;
using namespace modb::net;

namespace {

// No diretório do teste, não no temp: no Windows, connect num AF_UNIX sob
// %LOCALAPPDATA% (onde fica o temp) falha com WSAEINVAL em algumas máquinas.
std::filesystem::path socket_path(std::string_view tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::current_path() / ("modb-" + std::string{tag} + "-" + std::to_string(stamp) + ".sock");
}

} // namespace

int main() {
    TestSuite suite;

    // --- eco: bytes crus e frames do protocolo ---
    {
        const auto path = socket_path("echo");
        auto listener = NativeSocket::listen_local(path);
        suite.check(listener.has_value(), "listen_local cria o socket");
        if (!listener) {
            return suite.finish();
        }
        suite.check(std::filesystem::exists(path), "o socket aparece no sistema de arquivos");

        std::thread server{[&] {
            auto peer = listener->accept();
            if (!peer) {
                return;
            }
            auto message = recv_message(*peer);
            if (message) {
                (void)send_message(*peer, *message);
            }
        }};

        auto client = NativeSocket::connect_local(path);
        suite.check(client.has_value(), "connect_local conecta");
        if (client) {
            const OpCall call{.call_id = 7, .operation_id = "a.b", .args = std::vector<std::byte>(5000, std::byte{3})};
            suite.check(send_message(*client, call).has_value(), "frame enviado pelo socket local");
            auto echoed = recv_message(*client);
            const auto* back = echoed ? std::get_if<OpCall>(&*echoed) : nullptr;
            suite.check(back != nullptr && *back == call, "o frame volta igual");
        }
        server.join();
        (void)listener->close();
        NativeSocket::remove_local(path);
        suite.check(!std::filesystem::exists(path), "remove_local apaga o socket");
    }

    // --- um socket velho no caminho não impede o bind ---
    {
        const auto path = socket_path("stale");
        {
            auto first = NativeSocket::listen_local(path);
            suite.check(first.has_value(), "primeiro listen");
            // Fecha sem apagar: é o que sobra de um processo que caiu.
        }
        auto second = NativeSocket::listen_local(path);
        suite.check(second.has_value(), "listen sobre um socket velho funciona");
        NativeSocket::remove_local(path);
    }

    // --- close() de outra thread acorda o accept ---
    {
        const auto path = socket_path("stop");
        auto listener = NativeSocket::listen_local(path);
        suite.check(listener.has_value(), "listener para a parada");
        if (listener) {
            bool woke = false;
            std::thread waiter{[&] { woke = !listener->accept().has_value(); }};
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            (void)listener->close();
            waiter.join();
            suite.check(woke, "accept acorda com erro quando o listener fecha");
        }
        NativeSocket::remove_local(path);
    }

    // --- shutdown() de outra thread acorda uma leitura bloqueada ---
    {
        const auto path = socket_path("shutdown");
        auto listener = NativeSocket::listen_local(path);
        auto client = listener ? NativeSocket::connect_local(path) : Result<NativeSocket>{std::unexpected(listener.error())};
        auto peer = client ? listener->accept() : Result<NativeSocket>{std::unexpected(client.error())};
        suite.check(peer.has_value(), "par conectado para o shutdown");
        if (peer) {
            bool woke = false;
            std::thread reader{[&] { woke = !recv_message(*peer).has_value(); }};
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            (void)peer->shutdown();
            reader.join();
            suite.check(woke, "recv bloqueado acorda com shutdown");
        }
        NativeSocket::remove_local(path);
    }

    // --- caminho longo demais é recusado antes de criar o socket ---
    {
        const auto path = std::filesystem::current_path() / std::string(200, 'x');
        suite.check(!NativeSocket::listen_local(path), "caminho longo demais é recusado");
        suite.check(!NativeSocket::connect_local(socket_path("nobody")), "conectar sem ninguém escutando falha");
    }

    return suite.finish();
}
