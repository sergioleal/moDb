#include "modb/app/server_connection.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

// This is the object type the server streams back to the application client.
struct Item {
    std::string name;
    std::int64_t value{};
};

modb::object::BindingBuilder<Item> item_binding() {
    // Both the seed database and the server database use the same binding.
    modb::object::BindingBuilder<Item> builder{"Item"};
    builder.field<1>("name", &Item::name).field<2>("value", &Item::value);
    return builder;
}

std::filesystem::path example_path() {
    // Use a stable name so the README command is easy to inspect while debugging.
    return std::filesystem::temp_directory_path() / "ring0-server-phase-08.modb";
}

void cleanup(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
}

} // namespace

int main() {
    std::cout << "Objective: connect to a Ring0 server and collect objects with ServerConnection.\n";

    const auto path = example_path();
    cleanup(path);

    {
        // Seed the file before the network server opens it.
        auto created = modb::object::Database::create(path);
        if (!created) {
            std::cerr << created.error().message << '\n';
            return 1;
        }
        auto database = std::make_shared<modb::object::Database>(std::move(*created));
        auto attached = modb::object::DatabaseRegistry::instance().attach(database);
        if (!attached || !database->bind(item_binding())) {
            std::cerr << "failed to bind Item\n";
            return 1;
        }
        auto tx = database->begin();
        if (!tx || !database->create(*tx, Item{"alpha", 10}) ||
            !database->create(*tx, Item{"beta", 20}) || !tx->commit()) {
            std::cerr << "failed to seed database\n";
            return 1;
        }
        modb::object::DatabaseRegistry::instance().detach(*attached);
    }

    // Listen on port 0 so the OS chooses an available loopback port.
    auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
    if (!server) {
        std::cerr << server.error().message << '\n';
        return 1;
    }
    if (!server->database().bind(item_binding())) {
        std::cerr << "failed to bind server database\n";
        return 1;
    }
    auto item_type = server->database().type_id_of<Item>();
    if (!item_type) {
        std::cerr << "failed to resolve Item type id\n";
        return 1;
    }

    // serve_one handles exactly one client session in the background.
    std::thread acceptor([&server] { (void)server->serve_one(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // ServerConnection wraps handshake plus query collection for applications.
    auto connection = modb::app::ServerConnection::connect({
        .host = "127.0.0.1",
        .port = server->port(),
        .database_name = std::string{server->database_name()},
    });
    if (!connection) {
        std::cerr << connection.error().message << '\n';
        acceptor.join();
        return 1;
    }

    // QueryDescription uses the negotiated type id rather than a C++ template.
    auto rows = connection->collect(modb::net::QueryDescription{
        .type = *item_type,
        .limit = 10,
    });
    if (!rows) {
        std::cerr << rows.error().message << '\n';
        acceptor.join();
        return 1;
    }

    std::cout << "connected protocol " << connection->info().protocol_major << '.'
              << connection->info().protocol_minor << '\n';
    std::cout << "objects received: " << rows->size() << '\n';

    acceptor.join();
    cleanup(path);
    return rows->size() == 2 ? 0 : 1;
}
