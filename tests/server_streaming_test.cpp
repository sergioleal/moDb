#include "modb/net/native_socket.hpp"
#include "modb/net/protocol.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {

std::filesystem::path temp_db_path(const char* tag) {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("modb-8b-" + std::string{tag} + "-" + std::to_string(stamp) + ".modb");
}

} // namespace

int main() {
    TestSuite suite;
    using modb::net::Client;
    using modb::net::Compression;
    using modb::net::NativeSocket;
    using modb::net::Server;
    using modb::net::protocol_version;
    using modb::object::Database;

    const auto path = temp_db_path("handshake");
    std::error_code filesystem_error;
    std::filesystem::remove(path, filesystem_error);
    std::filesystem::remove(path.string() + ".wal", filesystem_error);
    {
        auto created = Database::create(path);
        suite.check(created.has_value(), "create temp database");
        if (!created) {
            return suite.finish();
        }
    }

    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "server listens on ephemeral port");
        if (!server) {
            return suite.finish();
        }
        suite.check(server->port() != 0, "ephemeral port is non-zero");

        const auto port = server->port();
        const auto db_name = std::string{server->database_name()};

        std::thread acceptor([&server, &suite] {
            auto status = server->serve_one();
            suite.check(status.has_value(), "serve_one completes handshake");
        });

        // Pequena espera para o accept estar bloqueado.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        auto hello_ok = Client::handshake("127.0.0.1", port, db_name);
        suite.check(hello_ok.has_value(), "client handshake succeeds");
        if (hello_ok) {
            suite.check(hello_ok->version == protocol_version, "HelloOk version matches");
            suite.check(hello_ok->selected_codec == Compression::none,
                        "HelloOk selects compression=none");
            suite.check(hello_ok->max_frame_bytes > 0, "HelloOk advertises max frame");
        }

        acceptor.join();

        // Segunda rodada: conexão limpa após a primeira.
        std::thread acceptor2([&server, &suite] {
            auto status = server->serve_one();
            suite.check(status.has_value(), "second serve_one completes");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto again = Client::handshake("127.0.0.1", port, db_name);
        suite.check(again.has_value(), "connection reusable for new handshake");
        acceptor2.join();
    }

    // Socket básico: connect rejeitado em porta que acabou de fechar.
    {
        auto listener = NativeSocket::listen("127.0.0.1", 0);
        suite.check(listener.has_value(), "temp listener for refuse test");
        if (listener) {
            auto closed_port = listener->local_port();
            suite.check(closed_port.has_value(), "temp listener reports port");
            suite.check(static_cast<bool>(listener->close()), "temp listener closes");
            if (closed_port) {
                auto refused = NativeSocket::connect("127.0.0.1", *closed_port);
                suite.check(!refused, "connect to closed port fails");
            }
        }
    }

    std::filesystem::remove(path, filesystem_error);
    std::filesystem::remove(path.string() + ".wal", filesystem_error);
    return suite.finish();
}
