// Catálogo de facades (Fase 11A): registro, listagem, lookup e identidade ≠ posição.

#include "modb/error.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/facade_descriptor.hpp"

#include "test_support.hpp"

#include <string>
#include <vector>

int main() {
    TestSuite suite;
    using modb::ErrorCode;
    using modb::ops::FacadeCatalog;
    using modb::ops::FacadeDescriptor;
    using modb::ops::FacadeMode;
    using modb::ops::MethodDescriptor;
    using modb::ops::OperationMode;
    using modb::ops::find_method;

    FacadeCatalog catalog;

    const FacadeDescriptor accounts{
        .facade_id = "accounts",
        .facade_version = 1,
        .mode = FacadeMode::read_write,
        .methods =
            {
                MethodDescriptor{.operation_id = "account.transfer",
                                 .method_version = 1,
                                 .mode = OperationMode::read_write},
                MethodDescriptor{.operation_id = "account.balance",
                                 .method_version = 1,
                                 .mode = OperationMode::read_only},
            },
    };
    const FacadeDescriptor finance{
        .facade_id = "finance",
        .facade_version = 1,
        .mode = FacadeMode::mixed,
        .methods =
            {
                MethodDescriptor{.operation_id = "finance.close-month",
                                 .method_version = 1,
                                 .mode = OperationMode::read_write},
            },
    };

    suite.check(catalog.register_facade(accounts).has_value(), "register accounts");
    suite.check(catalog.register_facade(finance).has_value(), "register finance");
    suite.check(catalog.size() == 2, "catalog size after two registers");
    suite.check(catalog.list().size() == 2, "list size matches");

    {
        auto found = catalog.find("accounts", 1);
        suite.check(found.has_value(), "find accounts v1");
        if (found) {
            suite.check(found->facade_id == "accounts", "found id");
            suite.check(found->methods.size() == 2, "found methods");
        }
    }

    {
        auto missing = catalog.find("orders", 1);
        suite.check(!missing && missing.error().code == ErrorCode::facade_not_found,
                    "unknown id → facade_not_found");
    }

    {
        auto wrong = catalog.find("accounts", 99);
        suite.check(!wrong && wrong.error().code == ErrorCode::incompatible_facade_version,
                    "wrong version → incompatible_facade_version");
    }

    {
        auto method = catalog.find_method("accounts", 1, "account.transfer");
        suite.check(method.has_value() && (*method)->operation_id == "account.transfer",
                    "find_method transfer");
        auto alien = catalog.find_method("accounts", 1, "finance.close-month");
        suite.check(!alien && alien.error().code == ErrorCode::facade_method_not_found,
                    "alien method → facade_method_not_found");
    }

    {
        auto dup = catalog.register_facade(accounts);
        suite.check(!dup && dup.error().code == ErrorCode::invalid_argument,
                    "duplicate facade_id+version rejected");
    }

    {
        auto empty = catalog.register_facade(FacadeDescriptor{.facade_id = ""});
        suite.check(!empty && empty.error().code == ErrorCode::invalid_argument,
                    "empty facade_id rejected");
    }

    // Posição no vetor ≠ identidade: reordenar não altera lookup por FacadeId.
    {
        FacadeCatalog shuffled;
        suite.check(shuffled.register_facade(finance).has_value(), "register finance first");
        suite.check(shuffled.register_facade(accounts).has_value(), "register accounts second");
        suite.check(shuffled.list()[0].facade_id == "finance", "finance is index 0");
        suite.check(shuffled.list()[1].facade_id == "accounts", "accounts is index 1");

        auto found = shuffled.find("accounts", 1);
        suite.check(found.has_value() && found->facade_id == "accounts",
                    "lookup by id ignores vector position");
        suite.check(found && found->methods.size() == 2, "accounts methods intact after reorder");

        auto by_index_id = shuffled.find(shuffled.list()[0].facade_id, 1);
        suite.check(by_index_id.has_value() && by_index_id->facade_id == "finance",
                    "index 0 is finance by id, not by former position");
    }

    {
        const auto& listed = catalog.list()[0];
        auto method = find_method(listed, "account.balance");
        suite.check(method.has_value() && (*method)->mode == OperationMode::read_only,
                    "descriptor find_method for balance");
    }

    return suite.finish();
}
