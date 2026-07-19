#include "examples/transfer_funds/transfer_funds.hpp"
#include "modb/net/client.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/module_manifest.hpp"
#include "modb/ops/operation_registry.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

std::filesystem::path temp_db_path(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("modb-9s-" + std::string{tag} + "-" + std::to_string(stamp) + ".modb");
}

void cleanup(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
}

modb::Result<void> bind_accounts(modb::object::Database& database) {
    modb::object::BindingBuilder<modb::examples::Account> builder{"Account"};
    builder.field<1>("owner", &modb::examples::Account::owner)
        .field<2>("balance", &modb::examples::Account::balance);
    return database.bind(std::move(builder));
}

} // namespace

int main() {
    TestSuite suite;
    using modb::ErrorCode;
    using modb::examples::Account;
    using modb::examples::TransferFunds;
    using modb::examples::register_transfer_funds_module;
    using modb::examples::transfer_funds_manifest;
    using modb::net::Client;
    using modb::net::Server;
    using modb::object::Database;
    using modb::object::DatabaseRegistry;
    using modb::ops::ModuleLoader;
    using modb::ops::OperationRegistry;

    const auto path = temp_db_path("net");
    cleanup(path);

    modb::object::ObjectId alice{};
    modb::object::ObjectId bob{};
    {
        auto created = Database::create(path);
        suite.check(created.has_value(), "create database");
        auto database = std::make_shared<Database>(std::move(*created));
        auto attached = DatabaseRegistry::instance().attach(database);
        suite.check(attached.has_value(), "attach");
        suite.check(bind_accounts(*database).has_value(), "bind");

        auto tx = database->begin();
        auto a = database->create(*tx, Account{"Alice", 100});
        auto b = database->create(*tx, Account{"Bob", 10});
        suite.check(a.has_value() && b.has_value(), "seed");
        alice = a->id();
        bob = b->id();
        suite.check(tx->commit().has_value(), "commit seed");

        DatabaseRegistry::instance().detach(*attached);
        database.reset();

        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "server listens");
        if (!server) {
            cleanup(path);
            return suite.finish();
        }
        suite.check(bind_accounts(server->database()).has_value(), "bind on server database");

        auto registry = std::make_shared<OperationRegistry>();
        const auto baseline = server->database().current_baseline()->id();
        auto manifest = transfer_funds_manifest(baseline);
        ModuleLoader loader;
        loader.admit_hash(manifest.hash);
        suite.check(loader
                        .load(manifest, baseline, *registry,
                              [](OperationRegistry& reg) {
                                  return register_transfer_funds_module(reg);
                              })
                        .has_value(),
                    "load module");
        server->set_operation_registry(registry);

        const auto port = server->port();
        const auto db_name = std::string{server->database_name()};
        std::thread acceptor([&server, &suite] {
            suite.check(server->serve_one().has_value(), "serve_one ok");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        {
            auto client = Client::connect("127.0.0.1", port, db_name);
            suite.check(client.has_value(), "client connects");
            if (client) {
                auto args = TransferFunds::encode_args(alice, bob, 40);
                auto payload = client->call(TransferFunds::k_id, *args);
                suite.check(payload.has_value(), "client.call transfer succeeds");

                auto fail_args = TransferFunds::encode_args(alice, bob, 1000);
                auto failed = client->call(TransferFunds::k_id, *fail_args);
                suite.check(!failed && failed.error().message.find("insufficient") != std::string::npos,
                            "client.call insufficient funds");
            }
        }
        acceptor.join();
    }

    // Reabre após commit: transferência presente (ciclo supervisor/WAL recovery).
    {
        auto opened = Database::open(path);
        suite.check(opened.has_value(), "reopen after network ops");
        if (opened) {
            auto database = std::make_shared<Database>(std::move(*opened));
            auto attached = DatabaseRegistry::instance().attach(database);
            suite.check(bind_accounts(*database).has_value(), "rebind");
            auto alice_v = database->materialize(*database->get<Account>(alice));
            auto bob_v = database->materialize(*database->get<Account>(bob));
            suite.check(alice_v && alice_v->balance == 60, "alice durable after reopen");
            suite.check(bob_v && bob_v->balance == 50, "bob durable after reopen");
            DatabaseRegistry::instance().detach(*attached);
        }
    }

    cleanup(path);
    return suite.finish();
}
