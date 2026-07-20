#include "modb/app/server_connection.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

struct Item {
    std::string name;
    std::int64_t value{};
};

modb::object::BindingBuilder<Item> item_builder() {
    modb::object::BindingBuilder<Item> builder{"Item"};
    builder.field<1>("name", &Item::name).field<2>("value", &Item::value);
    return builder;
}

std::filesystem::path temp_db_path() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("ring0-app-client-" + std::to_string(stamp) + ".modb");
}

void cleanup(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
}

} // namespace

int main() {
    TestSuite suite;

    const auto path = temp_db_path();
    cleanup(path);

    {
        auto created = modb::object::Database::create(path);
        suite.check(created.has_value(), "create database");
        if (!created) {
            cleanup(path);
            return suite.finish();
        }
        auto database = std::make_shared<modb::object::Database>(std::move(*created));
        auto attached = modb::object::DatabaseRegistry::instance().attach(database);
        suite.check(attached.has_value(), "attach database");
        suite.check(database->bind(item_builder()).has_value(), "bind item");

        auto tx = database->begin();
        suite.check(tx.has_value(), "begin seed tx");
        if (tx) {
            suite.check(database->create(*tx, Item{"alpha", 10}).has_value(), "create alpha");
            suite.check(database->create(*tx, Item{"beta", 20}).has_value(), "create beta");
            suite.check(tx->commit().has_value(), "commit seed tx");
        }
        if (attached) {
            modb::object::DatabaseRegistry::instance().detach(*attached);
        }
    }

    {
        auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "server listens");
        if (!server) {
            cleanup(path);
            return suite.finish();
        }
        auto item_bound = server->database().bind(item_builder());
        suite.check(item_bound.has_value(), "bind server item");
        auto item_type = server->database().type_id_of<Item>();
        suite.check(item_type.has_value(), "lookup server item type");

        const modb::app::ConnectionOptions options{
            .host = "127.0.0.1",
            .port = server->port(),
            .database_name = std::string{server->database_name()},
        };

        std::thread acceptor([&server, &suite] {
            suite.check(server->serve_one().has_value(), "serve one client");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        auto client = modb::app::ServerConnection::connect(options);
        suite.check(client.has_value(), "app client connects");
        if (client && item_type) {
            suite.check(client->info().protocol_major == modb::net::protocol_major,
                        "server info exposes protocol");
            auto objects = client->collect(modb::net::QueryDescription{
                .type = *item_type,
                .limit = 10,
            });
            suite.check(objects.has_value(), "collect query");
            suite.check(objects && objects->size() == 2, "collects seeded objects");
        }

        acceptor.join();
    }

    cleanup(path);
    return suite.finish();
}
