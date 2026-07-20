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
           ("ring0-phase-03-" +
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
    if (!account || !account->set<&Account::balance>(*tx, 125) || !tx->commit()) {
        std::cerr << "failed to update Account\n";
        cleanup(path);
        return 1;
    }

    auto current = database->materialize(*database->get<Account>(account->id()));
    std::cout << current->owner << " balance=" << current->balance << '\n';
    modb::object::DatabaseRegistry::instance().detach(*attached);
    cleanup(path);
    return current->balance == 125 ? 0 : 1;
}
