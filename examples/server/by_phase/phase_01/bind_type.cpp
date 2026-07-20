#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace {

struct Customer {
    std::string name;
    std::int64_t score{};
};

modb::object::BindingBuilder<Customer> customer_binding() {
    modb::object::BindingBuilder<Customer> builder{"Customer"};
    builder.field<1>("name", &Customer::name).field<2>("score", &Customer::score);
    return builder;
}

std::filesystem::path temp_path() {
    return std::filesystem::temp_directory_path() /
           ("ring0-phase-01-" +
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
    if (!created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*created));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached || !database->bind(customer_binding())) {
        std::cerr << "failed to bind Customer\n";
        cleanup(path);
        return 1;
    }
    auto type_id = database->type_id_of<Customer>();
    std::cout << "Customer type id: " << type_id->value << '\n';
    modb::object::DatabaseRegistry::instance().detach(*attached);
    cleanup(path);
    return 0;
}
