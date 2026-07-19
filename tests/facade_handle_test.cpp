#include "examples/transfer_funds/transfer_funds.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/facade_handle.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

struct AccountsFacade {
    static constexpr std::string_view k_id = "accounts";
    static constexpr std::uint32_t k_version = 1;
};

struct AccountsV2Facade {
    static constexpr std::string_view k_id = "accounts";
    static constexpr std::uint32_t k_version = 2;
};

struct MissingFacade {
    static constexpr std::string_view k_id = "missing";
    static constexpr std::uint32_t k_version = 1;
};

std::filesystem::path temp_db_path() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("modb-11b-facade-handle-" + std::to_string(stamp) + ".modb");
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
    using modb::examples::MigrationBumpBalance;
    using modb::examples::TransferFunds;
    using modb::object::Database;
    using modb::object::DatabaseRegistry;
    using modb::ops::FacadeCatalog;
    using modb::ops::FacadeDescriptor;
    using modb::ops::FacadeMode;
    using modb::ops::MethodDescriptor;
    using modb::ops::OperationMode;
    using modb::ops::OperationRegistry;
    using modb::ops::open_facade;

    const auto path = temp_db_path();
    cleanup(path);

    auto created = Database::create(path);
    suite.check(created.has_value(), "create database");
    if (!created) {
        return suite.finish();
    }
    auto database = std::make_shared<Database>(std::move(*created));
    auto attached = DatabaseRegistry::instance().attach(database);
    suite.check(attached.has_value(), "attach database");
    if (!attached) {
        cleanup(path);
        return suite.finish();
    }
    suite.check(bind_accounts(*database).has_value(), "bind accounts");

    modb::object::ObjectId alice{};
    modb::object::ObjectId bob{};
    {
        auto tx = database->begin();
        suite.check(tx.has_value(), "begin seed transaction");
        if (tx) {
            auto first = database->create(*tx, Account{"Alice", 100});
            auto second = database->create(*tx, Account{"Bob", 20});
            suite.check(first.has_value() && second.has_value(), "create seed accounts");
            if (first && second) {
                alice = first->id();
                bob = second->id();
            }
            suite.check(tx->commit().has_value(), "commit seed accounts");
        }
    }

    OperationRegistry registry;
    suite.check(
        registry.register_operation<TransferFunds>(std::string{TransferFunds::k_id}).has_value(),
        "register transfer operation");
    suite.check(registry
                    .register_operation<MigrationBumpBalance>(
                        std::string{MigrationBumpBalance::k_id})
                    .has_value(),
                "register alien operation");

    FacadeCatalog catalog;
    suite.check(
        catalog
            .register_facade(FacadeDescriptor{
                .facade_id = "accounts",
                .facade_version = 1,
                .mode = FacadeMode::read_write,
                .methods = {MethodDescriptor{.operation_id = std::string{TransferFunds::k_id},
                                             .method_version = 1,
                                             .mode = OperationMode::read_write}},
            })
            .has_value(),
        "register accounts facade");

    auto handle = open_facade<AccountsFacade>(catalog, registry, *database);
    suite.check(handle.has_value(), "open accounts facade");
    if (handle) {
        suite.check(handle->facade_id() == "accounts", "handle stores facade id");
        suite.check(handle->version() == 1, "handle stores negotiated version");

        auto result = handle->invoke<TransferFunds>(alice, bob, 30);
        suite.check(result.has_value(), "typed transfer invoke succeeds");

        auto alice_value = database->materialize(*database->get<Account>(alice));
        auto bob_value = database->materialize(*database->get<Account>(bob));
        suite.check(alice_value && alice_value->balance == 70, "transfer debits source");
        suite.check(bob_value && bob_value->balance == 50, "transfer credits destination");

        auto failed = handle->invoke<TransferFunds>(alice, bob, 1000);
        suite.check(!failed, "domain error propagates through handle");
        alice_value = database->materialize(*database->get<Account>(alice));
        bob_value = database->materialize(*database->get<Account>(bob));
        suite.check(alice_value && alice_value->balance == 70,
                    "failed invoke rolls back source");
        suite.check(bob_value && bob_value->balance == 50,
                    "failed invoke rolls back destination");

        auto alien =
            handle->invoke<MigrationBumpBalance>(std::vector{alice, bob}, std::int64_t{1});
        suite.check(!alien && alien.error().code == ErrorCode::facade_method_not_found,
                    "method outside facade is rejected before dispatch");
    }

    auto wrong_version = open_facade<AccountsV2Facade>(catalog, registry, *database);
    suite.check(!wrong_version &&
                    wrong_version.error().code == ErrorCode::incompatible_facade_version,
                "incompatible facade version rejected");

    auto missing = open_facade<MissingFacade>(catalog, registry, *database);
    suite.check(!missing && missing.error().code == ErrorCode::facade_not_found,
                "missing facade rejected");

    DatabaseRegistry::instance().detach(*attached);
    database.reset();
    cleanup(path);
    return suite.finish();
}
