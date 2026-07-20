#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace {

struct Account {
    std::string owner;
    std::int64_t balance{};
};

modb::object::BindingBuilder<Account> account_binding() {
    modb::object::BindingBuilder<Account> builder{"Account"};
    builder.field<1>("owner", &Account::owner).field<2>("balance", &Account::balance);
    return builder;
}

std::filesystem::path temp_path() {
    return std::filesystem::temp_directory_path() /
           ("ring0-phase-05-" +
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
    modb::object::ObjectId id{};
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
        auto account = database->create(*tx, Account{"Alice", 100});
        id = account->id();
        if (!account || !tx->commit()) {
            std::cerr << "failed to commit Account\n";
            cleanup(path);
            return 1;
        }
        modb::object::DatabaseRegistry::instance().detach(*attached);
    }

    auto opened = modb::object::Database::open(path);
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!database->bind(account_binding())) {
        std::cerr << "failed to rebind Account\n";
        cleanup(path);
        return 1;
    }
    auto account = database->materialize(*database->get<Account>(id));
    std::cout << account->owner << " recovered balance=" << account->balance << '\n';
    modb::object::DatabaseRegistry::instance().detach(*attached);
    cleanup(path);
    return account->balance == 100 ? 0 : 1;
}
