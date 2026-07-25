// Lesson 1 -- Binding Your First Type.
// See docs/training/en/01-binding-your-first-type/01-binding-your-first-type.md.
//
// This is the start of a course that chains lessons through one real,
// persistent database file: this lesson creates it, and every later
// lesson (a separate binary) reopens the SAME file and continues from
// wherever the previous lesson left off. See ../README.md for the
// details of that chain.

#include "modb/object/database.hpp"

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

// MODB_TRAINING_DIR is injected by CMakeLists.txt (modb_add_training_lesson)
// as the absolute path to docs/training/en, so every lesson finds the same
// file no matter what directory the binary is run from.
std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

} // namespace

int main() {
    std::cout << "Objective: bind an Employee type and register it in the catalog.\n";

    // Lesson 1 always starts the course fresh -- re-running it resets the
    // whole chain. Every later lesson only ever opens this file, never
    // creates or deletes it.
    const auto path = db_path();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);

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
    std::cout << "Employee type id = " << type_id->value << '\n';
    std::cout << "Database created at " << path.string() << "\n";

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}
