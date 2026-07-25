// Lesson 2 -- Persist and Reopen.
// Builds on Lesson 1 (lesson_01_binding.cpp) -- opens the SAME database
// file Lesson 1 created; this lesson never creates or deletes it.
// See docs/training/en/02-persist-and-reopen/02-persist-and-reopen.md.

#include "modb/object/database.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

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

constexpr modb::object::FieldId kNameField{1};

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

// Every lesson from here on is a SEPARATE binary with no shared in-process
// state, so instead of passing ObjectIds forward, each one looks employees
// up by name -- which is exactly why the index below exists.
modb::Result<modb::object::ObjectId> find_employee_id(modb::object::Database& database,
                                                       std::string_view name) {
    auto matches = database.indexed_object_ids<Employee>(
        kNameField, modb::object::AttributeValue{std::string{name}});
    if (!matches) {
        return std::unexpected(matches.error());
    }
    if (matches->empty()) {
        return std::unexpected(
            modb::Error{modb::ErrorCode::record_not_found, "no employee named " + std::string{name}});
    }
    return (*matches)[0];
}

} // namespace

int main() {
    std::cout << "Objective: persist real employees and read them back after a restart.\n";
    const auto path = db_path();

    // --- "First run": open the file Lesson 1 created, write employees. ---
    modb::object::ObjectId ana_id{};
    {
        auto opened = modb::object::Database::open(path);
        if (!opened) {
            std::cerr << opened.error().message
                      << " -- have you run Lesson 1 first? Expected a database at "
                      << path.string() << '\n';
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
        ana_id = ana->id();
        std::cout << "Wrote 3 employees (Ana=" << ana_id.value << ", Bruno=" << bruno->id().value
                  << ", Carla=" << carla->id().value << ")\n";

        // An index on `name` -- not on this lesson's own topic, but every
        // lesson from here on needs a way to find "Ana" again without a
        // shared in-process id, since each one is a separate program run.
        if (auto created = database->create_index<Employee>(kNameField); !created) {
            std::cerr << created.error().message << '\n';
            return 1;
        }

        modb::object::DatabaseRegistry::instance().detach(*attached);
        // `database` (the shared_ptr) drops here -- nothing else references
        // it, so the file is closed at the end of this scope.
    }

    // --- "Second run": reopen and read a record back, found by name. ---
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

        auto found_id = find_employee_id(*database, "Ana");
        if (!found_id) {
            std::cerr << found_id.error().message << '\n';
            return 1;
        }
        auto handle = database->get<Employee>(*found_id);
        if (!handle) {
            std::cerr << handle.error().message << '\n';
            return 1;
        }
        auto value = database->materialize(*handle);
        if (!value) {
            std::cerr << value.error().message << '\n';
            return 1;
        }
        std::cout << "After reopen, found \"Ana\" as employee " << found_id->value << " = "
                  << value->name << " (" << value->salary << ")\n";
        if (*found_id != ana_id) {
            std::cerr << "looked-up id does not match the id from the first run\n";
            return 1;
        }

        modb::object::DatabaseRegistry::instance().detach(*attached);
    }

    return 0;
}
