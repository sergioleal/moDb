#include "examples/transfer_funds/transfer_funds.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/module_manifest.hpp"
#include "modb/ops/operation_registry.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

std::filesystem::path temp_db_path(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("modb-9-" + std::string{tag} + "-" + std::to_string(stamp) + ".modb");
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
    using modb::ops::ModuleLoader;
    using modb::ops::OperationRegistry;
    using modb::ops::compute_manifest_hash;
    using modb::examples::register_transfer_funds_module;
    using modb::examples::transfer_funds_manifest;

    const auto path = temp_db_path("ops");
    cleanup(path);

    {
        auto created = Database::create(path);
        suite.check(created.has_value(), "create database");
        if (!created) {
            return suite.finish();
        }
        auto database = std::make_shared<Database>(std::move(*created));
        auto attached = DatabaseRegistry::instance().attach(database);
        suite.check(attached.has_value(), "attach database");
        suite.check(bind_accounts(*database).has_value(), "bind Account");

        modb::object::ObjectId alice{};
        modb::object::ObjectId bob{};
        {
            auto tx = database->begin();
            suite.check(tx.has_value(), "begin seed tx");
            auto a = database->create(*tx, Account{"Alice", 100});
            auto b = database->create(*tx, Account{"Bob", 20});
            suite.check(a.has_value() && b.has_value(), "seed accounts");
            if (a && b) {
                alice = a->id();
                bob = b->id();
            }
            suite.check(tx->commit().has_value(), "commit seed");
        }

        const auto baseline = database->current_baseline()->id();
        auto manifest = transfer_funds_manifest(baseline);
        ModuleLoader loader;
        loader.admit_hash(manifest.hash);

        OperationRegistry registry;
        suite.check(loader
                        .load(manifest, baseline, registry,
                              [](OperationRegistry& reg) {
                                  return register_transfer_funds_module(reg);
                              })
                        .has_value(),
                    "load transfer_funds module");

        // --- dispatch feliz ---
        {
            auto args = TransferFunds::encode_args(alice, bob, 30);
            suite.check(args.has_value(), "encode transfer args");
            auto result = registry.dispatch(TransferFunds::k_id, *args, *database);
            suite.check(result.has_value(), "happy transfer dispatches");

            auto alice_h = database->get<Account>(alice);
            auto bob_h = database->get<Account>(bob);
            auto alice_v = database->materialize(*alice_h);
            auto bob_v = database->materialize(*bob_h);
            suite.check(alice_v && alice_v->balance == 70, "alice debited");
            suite.check(bob_v && bob_v->balance == 50, "bob credited");
        }

        // --- saldo insuficiente ---
        {
            auto args = TransferFunds::encode_args(alice, bob, 1000);
            auto result = registry.dispatch(TransferFunds::k_id, *args, *database);
            suite.check(!result && result.error().message.find("insufficient") != std::string::npos,
                        "insufficient funds returns error");
            auto alice_v = database->materialize(*database->get<Account>(alice));
            auto bob_v = database->materialize(*database->get<Account>(bob));
            suite.check(alice_v && alice_v->balance == 70, "alice unchanged after failure");
            suite.check(bob_v && bob_v->balance == 50, "bob unchanged after failure");
        }

        // --- exceção do módulo ---
        {
            auto result = registry.dispatch("test.throw", {}, *database);
            suite.check(!result, "throwing operation fails");
            auto alice_v = database->materialize(*database->get<Account>(alice));
            suite.check(alice_v && alice_v->balance == 70, "balances intact after throw");
            // Motor continua utilizável.
            auto args = TransferFunds::encode_args(alice, bob, 5);
            suite.check(registry.dispatch(TransferFunds::k_id, *args, *database).has_value(),
                        "registry usable after throw");
            alice_v = database->materialize(*database->get<Account>(alice));
            suite.check(alice_v && alice_v->balance == 65, "follow-up transfer ok");
        }

        // --- id desconhecido ---
        {
            auto result = registry.dispatch("nao.existe", {}, *database);
            suite.check(!result && result.error().code == ErrorCode::operation_not_found,
                        "unknown operation id");
        }

        // --- manifesto incompatível ---
        {
            OperationRegistry empty;
            ModuleLoader strict;
            auto bad = manifest;
            bad.api_version = 999;
            bad.hash = compute_manifest_hash(bad);
            strict.admit_hash(bad.hash);
            auto status = strict.load(bad, baseline, empty, [](OperationRegistry&) -> modb::Result<void> {
                return {};
            });
            suite.check(!status && status.error().code == ErrorCode::incompatible_module,
                        "api_version mismatch rejected");
        }

        // --- hash não admitido ---
        {
            OperationRegistry empty;
            ModuleLoader strict;
            auto status = strict.load(manifest, baseline, empty, [](OperationRegistry&) -> modb::Result<void> {
                return {};
            });
            suite.check(!status && status.error().code == ErrorCode::incompatible_module,
                        "hash not in allowlist rejected");
        }

        // --- migração como operação ---
        {
            auto args = MigrationBumpBalance::encode_args({alice, bob}, 1);
            suite.check(args.has_value(), "encode migration args");
            suite.check(registry.dispatch(MigrationBumpBalance::k_id, *args, *database).has_value(),
                        "migration operation commits");
            auto alice_v = database->materialize(*database->get<Account>(alice));
            auto bob_v = database->materialize(*database->get<Account>(bob));
            suite.check(alice_v && alice_v->balance == 66, "alice migrated");
            suite.check(bob_v && bob_v->balance == 56, "bob migrated");
        }

        DatabaseRegistry::instance().detach(*attached);
    }

    cleanup(path);
    return suite.finish();
}
