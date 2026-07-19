#include "examples/accounts_facade/accounts_facade.hpp"
#include "examples/transfer_funds/transfer_funds.hpp"
#include "modb/net/client.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/facade_catalog.hpp"
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
           ("modb-11d-" + std::string{tag} + "-" + std::to_string(stamp) + ".modb");
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
    using modb::examples::AccountsFacade;
    using modb::examples::MigrationBumpBalance;
    using modb::examples::TransferFunds;
    using modb::examples::accounts_facade_manifest;
    using modb::examples::register_accounts_facade_module;
    using modb::net::Client;
    using modb::net::Server;
    using modb::object::Database;
    using modb::object::DatabaseRegistry;
    using modb::ops::FacadeCatalog;
    using modb::ops::ModuleLoader;
    using modb::ops::OperationRegistry;

    const auto path = temp_db_path("net");
    cleanup(path);

    modb::object::ObjectId alice{};
    modb::object::ObjectId bob{};
    {
        auto created = Database::create(path);
        suite.check(created.has_value(), "create database");
        if (!created) {
            cleanup(path);
            return suite.finish();
        }
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
    }

    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "server listens");
        if (!server) {
            cleanup(path);
            return suite.finish();
        }
        suite.check(bind_accounts(server->database()).has_value(), "bind on server");

        auto registry = std::make_shared<OperationRegistry>();
        auto catalog = std::make_shared<FacadeCatalog>();
        const auto baseline = server->database().current_baseline()->id();
        auto manifest = accounts_facade_manifest(baseline);
        ModuleLoader loader;
        loader.admit_hash(manifest.hash);
        suite.check(loader
                        .load(manifest, baseline, *registry, *catalog,
                              [](OperationRegistry& reg) {
                                  return register_accounts_facade_module(reg);
                              })
                        .has_value(),
                    "load module with facades from manifest");
        suite.check(catalog->size() == 1, "catalog has accounts facade");
        server->set_operation_registry(registry);
        server->set_facade_catalog(catalog);

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
                auto listed = client->list_facades();
                suite.check(listed.has_value() && listed->size() == 1, "list accounts facade");
                if (listed && !listed->empty()) {
                    suite.check((*listed)[0].facade_id == "accounts", "lists accounts id");
                    suite.check((*listed)[0].methods.size() == 1, "lists transfer method");
                }

                auto wrong = client->open_facade("accounts", 99);
                suite.check(wrong.has_value() && !wrong->ok &&
                                wrong->code == ErrorCode::incompatible_facade_version,
                            "reject incompatible version on wire");

                auto handle = client->open_facade<AccountsFacade>();
                suite.check(handle.has_value(), "open typed remote facade");
                if (handle) {
                    suite.check(handle->facade_id() == "accounts", "remote handle id");
                    suite.check(handle->version() == 1, "remote handle version");

                    auto ok = handle->invoke<TransferFunds>(alice, bob, 40);
                    suite.check(ok.has_value(), "remote invoke transfer commits");

                    auto failed = handle->invoke<TransferFunds>(alice, bob, 1000);
                    suite.check(!failed &&
                                    failed.error().message.find("insufficient") != std::string::npos,
                                "remote invoke insufficient funds rolls back");

                    auto alien = handle->invoke<MigrationBumpBalance>(
                        std::vector<modb::object::ObjectId>{alice}, std::int64_t{1});
                    suite.check(!alien && alien.error().code == ErrorCode::facade_method_not_found,
                                "alien method rejected on remote handle");
                }
            }
        }
        acceptor.join();
    }

    {
        auto opened = Database::open(path);
        suite.check(opened.has_value(), "reopen after remote facade ops");
        if (opened) {
            auto database = std::make_shared<Database>(std::move(*opened));
            auto attached = DatabaseRegistry::instance().attach(database);
            suite.check(bind_accounts(*database).has_value(), "rebind");
            auto alice_v = database->materialize(*database->get<Account>(alice));
            auto bob_v = database->materialize(*database->get<Account>(bob));
            suite.check(alice_v && alice_v->balance == 60, "alice durable after facade transfer");
            suite.check(bob_v && bob_v->balance == 50, "bob durable after facade transfer");
            DatabaseRegistry::instance().detach(*attached);
        }
    }

    cleanup(path);
    return suite.finish();
}
