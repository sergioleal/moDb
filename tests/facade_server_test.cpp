#include "examples/transfer_funds/transfer_funds.hpp"
#include "modb/net/client.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/operation.hpp"

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
           ("modb-11c-" + std::string{tag} + "-" + std::to_string(stamp) + ".modb");
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
    using modb::examples::TransferFunds;
    using modb::net::Client;
    using modb::net::Server;
    using modb::object::Database;
    using modb::ops::FacadeCatalog;
    using modb::ops::FacadeDescriptor;
    using modb::ops::FacadeMode;
    using modb::ops::MethodDescriptor;
    using modb::ops::OperationMode;

    const auto path = temp_db_path("net");
    cleanup(path);

    {
        auto created = Database::create(path);
        suite.check(created.has_value(), "create database");
        if (!created) {
            cleanup(path);
            return suite.finish();
        }
    }

    auto server = Server::listen(path, "127.0.0.1", 0);
    suite.check(server.has_value(), "server listens");
    if (!server) {
        cleanup(path);
        return suite.finish();
    }
    suite.check(bind_accounts(server->database()).has_value(), "bind Account");

    auto catalog = std::make_shared<FacadeCatalog>();
    suite.check(
        catalog
            ->register_facade(FacadeDescriptor{
                .facade_id = "accounts",
                .facade_version = 1,
                .mode = FacadeMode::read_write,
                .methods =
                    {
                        MethodDescriptor{.operation_id = std::string{TransferFunds::k_id},
                                         .method_version = 1,
                                         .mode = OperationMode::read_write},
                    },
            })
            .has_value(),
        "register accounts facade");
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
            suite.check(listed.has_value(), "list_facades succeeds");
            if (listed) {
                suite.check(listed->size() == 1, "lists one facade");
                suite.check((*listed)[0].facade_id == "accounts", "lists accounts id");
                suite.check((*listed)[0].facade_version == 1, "lists accounts version");
                suite.check((*listed)[0].methods.size() == 1, "lists accounts methods");
                suite.check((*listed)[0].methods[0].operation_id == TransferFunds::k_id,
                            "lists transfer method");
            }

            auto opened = client->open_facade("accounts", 1);
            suite.check(opened.has_value() && opened->ok, "open compatible version");
            if (opened) {
                suite.check(opened->facade_id == "accounts", "open echoes facade id");
                suite.check(opened->facade_version == 1, "open echoes version");
            }

            auto wrong = client->open_facade("accounts", 99);
            suite.check(wrong.has_value() && !wrong->ok, "open incompatible version fails");
            if (wrong) {
                suite.check(wrong->code == ErrorCode::incompatible_facade_version,
                            "incompatible_facade_version on wire");
                suite.check(wrong->facade_id == "accounts", "error echoes facade id");
                suite.check(wrong->facade_version == 99, "error echoes requested version");
            }

            auto missing = client->open_facade("unknown", 1);
            suite.check(missing.has_value() && !missing->ok, "open missing facade fails");
            if (missing) {
                suite.check(missing->code == ErrorCode::facade_not_found,
                            "facade_not_found on wire");
            }
        }
    }

    acceptor.join();
    cleanup(path);
    return suite.finish();
}
