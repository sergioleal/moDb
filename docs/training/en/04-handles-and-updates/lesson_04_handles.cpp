// Lesson 4 -- Handles and Updates.
// Builds on Lesson 3 (lesson_03_transactions.cpp).
// See docs/training/en/04-handles-and-updates/04-handles-and-updates.md.

#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace {

// The shape Lessons 1-3 wrote to disk: two fields.
struct Employee {
    std::string name;
    std::int64_t salary{};
};

modb::object::BindingBuilder<Employee> employee_binding() {
    modb::object::BindingBuilder<Employee> builder{"Employee"};
    builder.field<1>("name", &Employee::name).field<2>("salary", &Employee::salary);
    return builder;
}

// This lesson's shape: the same catalog type ("Employee"), one field wider.
// Field ids 1 and 2 are unchanged; id 3 is new, with a default for records
// written before this lesson existed.
struct EmployeeV2 {
    std::string name;
    std::int64_t salary{};
    std::string country;
};

modb::object::BindingBuilder<EmployeeV2> employee_v2_binding() {
    modb::object::BindingBuilder<EmployeeV2> builder{"Employee"};
    builder.field<1>("name", &EmployeeV2::name)
        .field<2>("salary", &EmployeeV2::salary)
        .field<3>("country", &EmployeeV2::country, "BR");
    return builder;
}

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

int lesson_02_persist_and_reopen(const std::filesystem::path& path, DirectoryIds& ids) {
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
    }
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

int print_salary(modb::object::Database& database, modb::object::ObjectId id,
                 std::string_view label) {
    auto handle = database.get<Employee>(id);
    if (!handle) {
        std::cerr << handle.error().message << '\n';
        return 1;
    }
    auto value = database.materialize(*handle);
    if (!value) {
        std::cerr << value.error().message << '\n';
        return 1;
    }
    std::cout << "  " << label << ": " << value->name << " = " << value->salary << '\n';
    return 0;
}

int lesson_03_transactions(const std::filesystem::path& path, const DirectoryIds& ids) {
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

    std::cout << "Lesson 3: committed raise for Bruno\n";
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto handle = database->get<Employee>(ids.bruno);
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
    if (print_salary(*database, ids.bruno, "Bruno after committed raise") != 0) {
        return 1;
    }

    std::cout << "Lesson 3: uncommitted raise for Carla (deliberately not committed)\n";
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto handle = database->get<Employee>(ids.carla);
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
    }
    if (print_salary(*database, ids.carla, "Carla after scope exit (should be unchanged)") != 0) {
        return 1;
    }

    std::cout << "Lesson 3: averaging Ana and Bruno's salaries in one transaction\n";
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto ana_handle = database->get<Employee>(ids.ana);
        auto bruno_handle = database->get<Employee>(ids.bruno);
        if (!ana_handle || !bruno_handle) {
            std::cerr << "failed to read Ana/Bruno\n";
            return 1;
        }
        auto ana_value = database->materialize(*ana_handle);
        auto bruno_value = database->materialize(*bruno_handle);
        if (!ana_value || !bruno_value) {
            std::cerr << "failed to materialize Ana/Bruno\n";
            return 1;
        }
        const auto average = (ana_value->salary + bruno_value->salary) / 2;
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
    if (print_salary(*database, ids.ana, "Ana after averaging") != 0) {
        return 1;
    }
    if (print_salary(*database, ids.bruno, "Bruno after averaging") != 0) {
        return 1;
    }

    std::cout << "Lesson 3: attempting a second transaction while one is open\n";
    {
        auto first = database->begin();
        if (!first) {
            std::cerr << first.error().message << '\n';
            return 1;
        }
        auto second = database->begin();
        if (second) {
            std::cerr << "expected the second begin() to fail, but it succeeded\n";
            return 1;
        }
        std::cout << "  second begin() failed as expected: " << second.error().message << '\n';
    }

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}

// Lesson 4: evolve the schema (add `country`, defaulted for old records),
// then use Handle<T>::set instead of manual materialize/update for a raise.
int lesson_04_handles(const std::filesystem::path& path, const DirectoryIds& ids) {
    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << opened.error().message << '\n';
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached) {
        std::cerr << "failed to attach database\n";
        return 1;
    }
    // Divergent shape vs. the "Employee" type Lessons 1-3 registered ->
    // Database::bind adopts the name, sees the structure changed, and
    // registers a new TypeDefinition + Baseline. Ana/Bruno/Carla, written
    // under the old 2-field shape, are untouched on disk.
    if (!database->bind(employee_v2_binding())) {
        std::cerr << "failed to bind EmployeeV2\n";
        return 1;
    }

    auto ana_handle = database->get<EmployeeV2>(ids.ana);
    if (!ana_handle) {
        std::cerr << ana_handle.error().message << '\n';
        return 1;
    }
    auto ana_value = database->materialize(*ana_handle);
    if (!ana_value) {
        std::cerr << ana_value.error().message << '\n';
        return 1;
    }
    std::cout << "Lesson 4: Ana read through the new binding = " << ana_value->name << " ("
              << ana_value->salary << ", country=" << ana_value->country
              << ") -- country came from the declared default, not from disk\n";

    std::cout << "Lesson 4: raise for Ana via Handle::set (not manual materialize/update)\n";
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        if (auto updated = ana_handle->set<&EmployeeV2::salary>(*tx, ana_value->salary + 2000);
            !updated) {
            std::cerr << updated.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit Ana's raise\n";
            return 1;
        }
    }
    auto ana_after = database->materialize(*ana_handle);
    if (!ana_after) {
        std::cerr << ana_after.error().message << '\n';
        return 1;
    }
    std::cout << "  Ana after Handle::set raise: " << ana_after->name << " ("
              << ana_after->salary << ", country=" << ana_after->country
              << ") -- now physically stored in the new 3-field shape\n";

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}

} // namespace

int main() {
    std::cout << "Objective: evolve the schema and update through a typed Handle.\n";
    const auto path = temp_path();
    cleanup(path);

    if (const auto status = lesson_01_bind_type(path); status != 0) {
        cleanup(path);
        return status;
    }
    DirectoryIds ids;
    if (const auto status = lesson_02_persist_and_reopen(path, ids); status != 0) {
        cleanup(path);
        return status;
    }
    if (const auto status = lesson_03_transactions(path, ids); status != 0) {
        cleanup(path);
        return status;
    }
    const auto status = lesson_04_handles(path, ids);

    cleanup(path);
    return status;
}
