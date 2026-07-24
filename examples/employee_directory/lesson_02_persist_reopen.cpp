// Lesson 2 -- Persist and Reopen.
// Builds on Lesson 1 (examples/employee_directory/lesson_01_binding.cpp).
// See docs/training/en/02-persist-and-reopen.md.
//
// The whole program operates on ONE directory file across all lessons --
// Lesson 1 creates it, every later lesson reopens and extends it. That's
// what "each lesson takes the previous one's result as a starting point"
// means literally here. `DirectoryIds` accumulates the ids later lessons
// need to refer back to specific employees.

#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

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

// Ids handed from one lesson to the next; grows as later lessons introduce
// more employees, departments, etc.
struct DirectoryIds {
    modb::object::ObjectId ana{};
    modb::object::ObjectId bruno{};
    modb::object::ObjectId carla{};
};

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

// Lesson 1: create the directory file and bind Employee. Nothing is
// persisted yet -- that's this lesson.
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

// Lesson 2: real records that survive closing and reopening the directory.
// Two scoped blocks simulate two separate program runs against the same
// file Lesson 1 created.
int lesson_02_persist_and_reopen(const std::filesystem::path& path, DirectoryIds& ids) {
    // --- "First run": open the file Lesson 1 created, write employees. ---
    {
        auto opened = modb::object::Database::open(path);
        if (!opened) {
            std::cerr << opened.error().message << '\n';
            return 1;
        }
        auto database = std::make_shared<modb::object::Database>(std::move(*opened));
        auto attached = modb::object::DatabaseRegistry::instance().attach(database);
        if (!attached || !database->bind(employee_binding())) {
            std::cerr << "failed to bind Employee\n";
            return 1;
        }

        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto ana = database->create(*tx, Employee{"Ana", 12000});
        auto bruno = database->create(*tx, Employee{"Bruno", 9500});
        auto carla = database->create(*tx, Employee{"Carla", 15000});
        if (!ana || !bruno || !carla) {
            std::cerr << "failed to create employees\n";
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit employees\n";
            return 1;
        }
        ids.ana = ana->id();
        ids.bruno = bruno->id();
        ids.carla = carla->id();
        std::cout << "Lesson 2: wrote 3 employees (Ana=" << ids.ana.value
                  << ", Bruno=" << ids.bruno.value << ", Carla=" << ids.carla.value << ")\n";

        modb::object::DatabaseRegistry::instance().detach(*attached);
        // `database` (the shared_ptr) drops here -- nothing else references
        // it, so the file is closed at the end of this scope.
    }

    // --- "Second run": reopen and read a record back. ---
    {
        auto opened = modb::object::Database::open(path);
        if (!opened) {
            std::cerr << opened.error().message << '\n';
            return 1;
        }
        auto database = std::make_shared<modb::object::Database>(std::move(*opened));
        auto attached = modb::object::DatabaseRegistry::instance().attach(database);
        if (!attached || !database->bind(employee_binding())) {
            std::cerr << "failed to re-bind Employee after reopen\n";
            return 1;
        }

        auto handle = database->get<Employee>(ids.ana);
        if (!handle) {
            std::cerr << handle.error().message << '\n';
            return 1;
        }
        auto value = database->materialize(*handle);
        if (!value) {
            std::cerr << value.error().message << '\n';
            return 1;
        }
        std::cout << "Lesson 2: after reopen, employee " << ids.ana.value << " = "
                  << value->name << " (" << value->salary << ")\n";

        modb::object::DatabaseRegistry::instance().detach(*attached);
    }

    return 0;
}

} // namespace

int main() {
    std::cout << "Objective: persist real employees and read them back after a restart.\n";
    const auto path = temp_path();
    cleanup(path);

    if (const auto status = lesson_01_bind_type(path); status != 0) {
        cleanup(path);
        return status;
    }

    DirectoryIds ids;
    const auto status = lesson_02_persist_and_reopen(path, ids);

    cleanup(path);
    return status;
}
