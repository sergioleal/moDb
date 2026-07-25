// Lesson 3 -- Transactions.
// Builds on Lesson 2 (lesson_02_persist_reopen.cpp) -- opens the SAME
// database file, now containing Ana/Bruno/Carla.
// See docs/training/en/03-transactions/03-transactions.md.

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

modb::Result<std::int64_t> salary_of(modb::object::Database& database, modb::object::ObjectId id) {
    auto handle = database.get<Employee>(id);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    auto value = database.materialize(*handle);
    if (!value) {
        return std::unexpected(value.error());
    }
    return value->salary;
}

} // namespace

int main() {
    std::cout << "Objective: commit a raise properly, show an uncommitted one rolling back, "
                 "and prove single-writer.\n";
    const auto path = db_path();

    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << opened.error().message
                  << " -- have you run Lessons 1-2 first? Expected a database at " << path.string()
                  << '\n';
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached || !database->bind(employee_binding())) {
        std::cerr << "failed to bind Employee\n";
        return 1;
    }

    auto bruno_id = find_employee_id(*database, "Bruno");
    auto carla_id = find_employee_id(*database, "Carla");
    auto ana_id = find_employee_id(*database, "Ana");
    if (!bruno_id || !carla_id || !ana_id) {
        std::cerr << "failed to look up Ana/Bruno/Carla\n";
        return 1;
    }

    // --- A committed raise. ---
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto handle = database->get<Employee>(*bruno_id);
        if (!handle) {
            std::cerr << handle.error().message << '\n';
            return 1;
        }
        auto value = database->materialize(*handle);
        if (!value) {
            std::cerr << value.error().message << '\n';
            return 1;
        }
        value->salary += 1000;
        if (auto updated = database->update(*tx, *handle, *value); !updated) {
            std::cerr << updated.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit Bruno's raise\n";
            return 1;
        }
    }
    auto bruno_after_commit = salary_of(*database, *bruno_id);
    if (!bruno_after_commit) {
        std::cerr << bruno_after_commit.error().message << '\n';
        return 1;
    }
    std::cout << "Committed raise for Bruno: salary = " << *bruno_after_commit << '\n';

    // --- A deliberately uncommitted raise. ---
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto handle = database->get<Employee>(*carla_id);
        if (!handle) {
            std::cerr << handle.error().message << '\n';
            return 1;
        }
        auto value = database->materialize(*handle);
        if (!value) {
            std::cerr << value.error().message << '\n';
            return 1;
        }
        value->salary += 5000;
        if (auto updated = database->update(*tx, *handle, *value); !updated) {
            std::cerr << updated.error().message << '\n';
            return 1;
        }
        // `tx` goes out of scope here without commit() -- rollback-on-scope-exit.
    }
    auto carla_after_scope = salary_of(*database, *carla_id);
    if (!carla_after_scope) {
        std::cerr << carla_after_scope.error().message << '\n';
        return 1;
    }
    std::cout << "Uncommitted raise for Carla (deliberately not committed): salary is still "
              << *carla_after_scope << '\n';

    // --- One transaction touching two employees: average their salaries. ---
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto ana_handle = database->get<Employee>(*ana_id);
        auto bruno_handle = database->get<Employee>(*bruno_id);
        if (!ana_handle || !bruno_handle) {
            std::cerr << "failed to look up Ana/Bruno\n";
            return 1;
        }
        auto ana_value = database->materialize(*ana_handle);
        auto bruno_value = database->materialize(*bruno_handle);
        if (!ana_value || !bruno_value) {
            std::cerr << "failed to materialize Ana/Bruno\n";
            return 1;
        }
        const std::int64_t average = (ana_value->salary + bruno_value->salary) / 2;
        ana_value->salary = average;
        bruno_value->salary = average;
        if (auto updated = database->update(*tx, *ana_handle, *ana_value); !updated) {
            std::cerr << updated.error().message << '\n';
            return 1;
        }
        if (auto updated = database->update(*tx, *bruno_handle, *bruno_value); !updated) {
            std::cerr << updated.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit the average\n";
            return 1;
        }
    }
    auto ana_after_average = salary_of(*database, *ana_id);
    auto bruno_after_average = salary_of(*database, *bruno_id);
    if (!ana_after_average || !bruno_after_average) {
        std::cerr << "failed to read back the averaged salaries\n";
        return 1;
    }
    std::cout << "Averaged Ana and Bruno's salaries in one transaction: Ana = "
              << *ana_after_average << ", Bruno = " << *bruno_after_average << '\n';

    // --- A second transaction while one is already open. ---
    {
        auto first = database->begin();
        if (!first) {
            std::cerr << first.error().message << '\n';
            return 1;
        }
        auto second = database->begin();
        if (second) {
            std::cerr << "expected a second begin() to fail, but it succeeded\n";
            return 1;
        }
        std::cout << "A second begin() while one is already open failed as expected: "
                  << second.error().message << '\n';
    }

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}
