#include "modb/app/server_connection.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

namespace {

std::filesystem::path temp_path() {
    return std::filesystem::temp_directory_path() /
           ("ring0-phase-10-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".modb");
}

void cleanup(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
}

} // namespace

int main() {
    const auto path = temp_path();
    cleanup(path);
    {
        auto created = modb::object::Database::create(path);
        if (!created) {
            std::cerr << created.error().message << '\n';
            return 1;
        }
    }
    auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
    if (!server) {
        std::cerr << server.error().message << '\n';
        cleanup(path);
        return 1;
    }
    std::thread acceptor([&server] { (void)server->serve_one(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    auto info = modb::app::ServerConnection::handshake({
        .host = "127.0.0.1",
        .port = server->port(),
        .database_name = std::string{server->database_name()},
    });
    acceptor.join();
    if (!info) {
        std::cerr << info.error().message << '\n';
        cleanup(path);
        return 1;
    }
    std::cout << "protocol " << info->protocol_major << '.' << info->protocol_minor
              << " max_streams=" << info->max_concurrent_streams << '\n';
    cleanup(path);
    return 0;
}
