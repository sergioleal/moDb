#include "examples/accounts_facade/accounts_facade.hpp"
#include "examples/transfer_funds/transfer_funds.hpp"
#include "modb/app/server_connection.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/module_manifest.hpp"
#include "modb/ops/operation_registry.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

namespace {

modb::object::BindingBuilder<modb::examples::Account> account_binding() {
    modb::object::BindingBuilder<modb::examples::Account> builder{"Account"};
    builder.field<1>("owner", &modb::examples::Account::owner)
        .field<2>("balance", &modb::examples::Account::balance);
    return builder;
}

std::filesystem::path temp_path() {
    return std::filesystem::temp_directory_path() /
           ("ring0-phase-11-" +
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
    modb::object::ObjectId alice{};
    modb::object::ObjectId bob{};
    {
        auto created = modb::object::Database::create(path);
        auto database = std::make_shared<modb::object::Database>(std::move(*created));
        auto attached = modb::object::DatabaseRegistry::instance().attach(database);
        if (!database->bind(account_binding())) {
            std::cerr << "failed to bind Account\n";
            cleanup(path);
            return 1;
        }
        auto tx = database->begin();
        alice = database->create(*tx, modb::examples::Account{"Alice", 100})->id();
        bob = database->create(*tx, modb::examples::Account{"Bob", 10})->id();
        if (!tx->commit()) {
            std::cerr << "failed to seed accounts\n";
            cleanup(path);
            return 1;
        }
        modb::object::DatabaseRegistry::instance().detach(*attached);
    }

    auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
    if (!server->database().bind(account_binding())) {
        std::cerr << "failed to bind server Account\n";
        cleanup(path);
        return 1;
    }
    auto registry = std::make_shared<modb::ops::OperationRegistry>();
    auto catalog = std::make_shared<modb::ops::FacadeCatalog>();
    modb::ops::ModuleLoader loader;
    const auto baseline = server->database().current_baseline()->id();
    const auto manifest = modb::examples::accounts_facade_manifest(baseline);
    loader.admit_hash(manifest.hash);
    auto loaded =
        loader.load(manifest, baseline, *registry, *catalog, [](modb::ops::OperationRegistry& reg) {
        return modb::examples::register_accounts_facade_module(reg);
    });
    if (!loaded) {
        std::cerr << loaded.error().message << '\n';
        cleanup(path);
        return 1;
    }
    server->set_operation_registry(registry);
    server->set_facade_catalog(catalog);

    std::thread acceptor([&server] { (void)server->serve_one(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    auto connection = modb::app::ServerConnection::connect({
        .host = "127.0.0.1",
        .port = server->port(),
        .database_name = std::string{server->database_name()},
    });
    auto handle = connection->open_facade<modb::examples::AccountsFacade>();
    auto result = handle->invoke<modb::examples::TransferFunds>(alice, bob, 25);
    acceptor.join();
    if (!result) {
        std::cerr << result.error().message << '\n';
        cleanup(path);
        return 1;
    }
    std::cout << "remote accounts facade invoked\n";
    cleanup(path);
    return 0;
}
