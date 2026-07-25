// Lesson 4 -- Handles and Updates.
// Builds on Lesson 3 (lesson_03_transactions.cpp) -- opens the SAME
// database file and evolves its schema.
// See docs/training/en/04-handles-and-updates/04-handles-and-updates.md.

#include "modb/object/database.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

// The Lesson 1-3 shape. Records already on disk look like this; binding
// EmployeeV2 below (same catalog name "Employee", one more field) is what
// triggers real schema evolution against them.
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

constexpr modb::object::FieldId kNameField{1};

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

modb::Result<modb::object::ObjectId> find_employee_id(modb::object::Database& database,
                                                       std::string_view name) {
    auto matches = database.indexed_object_ids<EmployeeV2>(
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
    std::cout << "Objective: evolve the schema and update through a typed Handle.\n";
    const auto path = db_path();

    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << opened.error().message
                  << " -- have you run Lessons 1-3 first? Expected a database at " << path.string()
                  << '\n';
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached || !database->bind(employee_v2_binding())) {
        std::cerr << "failed to bind the evolved Employee\n";
        return 1;
    }

    auto ana_id = find_employee_id(*database, "Ana");
    if (!ana_id) {
        std::cerr << ana_id.error().message << '\n';
        return 1;
    }

    auto ana_handle = database->get<EmployeeV2>(*ana_id);
    if (!ana_handle) {
        std::cerr << ana_handle.error().message << '\n';
        return 1;
    }
    auto ana_value = database->materialize(*ana_handle);
    if (!ana_value) {
        std::cerr << ana_value.error().message << '\n';
        return 1;
    }
    std::cout << "Ana read through the new binding = " << ana_value->name << " ("
              << ana_value->salary << ", country=" << ana_value->country
              << ") -- country came from the declared default, not from disk\n";

    // Raise via Handle::set instead of the manual materialize/update pattern
    // from Lesson 3.
    auto tx = database->begin();
    if (!tx) {
        std::cerr << tx.error().message << '\n';
        return 1;
    }
    if (auto set = ana_handle->set<&EmployeeV2::salary>(*tx, ana_value->salary + 2000); !set) {
        std::cerr << set.error().message << '\n';
        return 1;
    }
    if (!tx->commit()) {
        std::cerr << "failed to commit Ana's raise\n";
        return 1;
    }

    auto ana_after = database->materialize(*ana_handle);
    if (!ana_after) {
        std::cerr << ana_after.error().message << '\n';
        return 1;
    }
    std::cout << "Raise for Ana via Handle::set: " << ana_after->name << " (" << ana_after->salary
              << ", country=" << ana_after->country
              << ") -- now physically stored in the new 3-field shape\n";

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}
