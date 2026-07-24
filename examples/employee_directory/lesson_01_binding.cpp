// Lesson 1 -- Binding Your First Type.
// See docs/training/en/01-binding-your-first-type.md.

#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace {

struct Employee {
    std::string name;
    std::int64_t salary{};
};

modb::object::BindingBuilder<Employee> employee_binding() {
    modb::object::BindingBuilder<Employee> builder{"Employee"};
    builder.field<1>("name", &Employee::name).field<2>("salary", &Employee::salary);
    return builder;
}

std::filesystem::path temp_path() {
    return std::filesystem::temp_directory_path() /
           ("employee-directory-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".modb");
}

void cleanup(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
}

// Lesson 1: bind Employee and prove it's known to the catalog. Nothing is
// persisted yet -- that's Lesson 2.
int lesson_01_bind_type(const std::filesystem::path& path) {
    auto created = modb::object::Database::create(path);
    if (!created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*created));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached || !database->bind(employee_binding())) {
        std::cerr << "failed to bind Employee\n";
        return 1;
    }

    auto type_id = database->type_id_of<Employee>();
    if (!type_id) {
        std::cerr << type_id.error().message << '\n';
        return 1;
    }
    std::cout << "Lesson 1: Employee type id = " << type_id->value << '\n';

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}

} // namespace

int main() {
    std::cout << "Objective: bind an Employee type and register it in the catalog.\n";
    const auto path = temp_path();
    cleanup(path);

    const auto status = lesson_01_bind_type(path);

    cleanup(path);
    return status;
}
