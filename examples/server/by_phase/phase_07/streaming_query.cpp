#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace {

struct Item {
    std::string name;
    std::int64_t value{};
};

modb::object::BindingBuilder<Item> item_binding() {
    modb::object::BindingBuilder<Item> builder{"Item"};
    builder.field<1>("name", &Item::name).field<2>("value", &Item::value);
    return builder;
}

std::filesystem::path temp_path() {
    return std::filesystem::temp_directory_path() /
           ("ring0-phase-07-" +
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
    if (!database->bind(item_binding())) {
        std::cerr << "failed to bind Item\n";
        cleanup(path);
        return 1;
    }

    auto tx = database->begin();
    for (int i = 0; i < 6; ++i) {
        if (!database->create(*tx, Item{"item-" + std::to_string(i), i})) {
            std::cerr << "failed to create Item\n";
            cleanup(path);
            return 1;
        }
    }
    if (!tx->commit()) {
        std::cerr << "failed to commit Items\n";
        cleanup(path);
        return 1;
    }

    int count = 0;
    for (auto& result :
         database->query<Item>().where([](const Item& item) { return item.value % 2 == 0; })
             .limit(2)
             .stream()) {
        if (!result) {
            std::cerr << result.error().message << '\n';
            return 1;
        }
        std::cout << result->name << '\n';
        ++count;
    }
    modb::object::DatabaseRegistry::instance().detach(*attached);
    cleanup(path);
    return count == 2 ? 0 : 1;
}
