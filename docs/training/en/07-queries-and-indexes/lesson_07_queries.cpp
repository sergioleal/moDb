// Lesson 7 -- Searching with Queries and Indexes.
// Builds on Lesson 6 (lesson_06_snapshots.cpp) -- opens the SAME database
// file; the schema is unchanged.
// See docs/training/en/07-queries-and-indexes/07-queries-and-indexes.md.

#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"

#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

using modb::object::AttributeValue;
using modb::object::BlobId;
using modb::object::FieldId;
using modb::object::OwnedRef;
using modb::object::Ref;

namespace {

struct Department {
    std::string name;
};

modb::object::BindingBuilder<Department> department_binding() {
    modb::object::BindingBuilder<Department> builder{"Department"};
    builder.field<1>("name", &Department::name);
    return builder;
}

struct EmergencyContact {
    std::string name;
    std::string phone;
};

modb::object::BindingBuilder<EmergencyContact> emergency_contact_binding() {
    modb::object::BindingBuilder<EmergencyContact> builder{"EmergencyContact"};
    builder.field<1>("name", &EmergencyContact::name).field<2>("phone", &EmergencyContact::phone);
    return builder;
}

struct Project {
    std::string name;
};

modb::object::BindingBuilder<Project> project_binding() {
    modb::object::BindingBuilder<Project> builder{"Project"};
    builder.field<1>("name", &Project::name);
    return builder;
}

struct EmployeeV3 {
    std::string name;
    std::int64_t salary{};
    std::string country;
    Ref<Department> department{};
    OwnedRef<EmergencyContact> emergency_contact{};
    BlobId projects{};
};

modb::object::BindingBuilder<EmployeeV3> employee_v3_binding() {
    modb::object::BindingBuilder<EmployeeV3> builder{"Employee"};
    builder.field<1>("name", &EmployeeV3::name)
        .field<2>("salary", &EmployeeV3::salary)
        .field<3>("country", &EmployeeV3::country, "BR")
        .field<4>("department", &EmployeeV3::department, Ref<Department>{})
        .field<5>("emergency_contact", &EmployeeV3::emergency_contact, OwnedRef<EmergencyContact>{})
        .field<6>("projects", &EmployeeV3::projects, BlobId{});
    return builder;
}

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

void print_plan(std::string_view label, const modb::query::QueryPlan& plan) {
    std::cout << "  " << label << ": access=" << plan.access_name()
              << " index_available=" << (plan.index_available ? "true" : "false") << '\n';
}

} // namespace

int main() {
    std::cout << "Objective: search employees with a table scan, then an index, then top-k.\n";
    const auto path = db_path();

    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << opened.error().message
                  << " -- have you run Lessons 1-6 first? Expected a database at " << path.string()
                  << '\n';
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached || !database->bind(employee_v3_binding()) || !database->bind(department_binding()) ||
        !database->bind(emergency_contact_binding()) || !database->bind(project_binding())) {
        std::cerr << "failed to bind Lesson 7 types\n";
        return 1;
    }

    constexpr FieldId salary_field{2};
    const auto max_salary = std::numeric_limits<std::int64_t>::max();

    // --- Before an index: a table scan. ---
    {
        auto query = database->query<EmployeeV3>().between(
            salary_field, AttributeValue{std::int64_t{15000}}, AttributeValue{max_salary});
        print_plan("employees earning >= 15000 (no index yet)", query.plan());
        std::cout << "  results:";
        for (auto& result : std::move(query).stream()) {
            if (!result) {
                std::cerr << result.error().message << '\n';
                return 1;
            }
            std::cout << ' ' << result->name;
        }
        std::cout << '\n';
    }

    // --- Create the index, then run the exact same search again. ---
    if (auto created = database->create_index<EmployeeV3>(salary_field); !created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    std::cout << "Created an index on Employee.salary\n";
    {
        auto query = database->query<EmployeeV3>().between(
            salary_field, AttributeValue{std::int64_t{15000}}, AttributeValue{max_salary});
        print_plan("employees earning >= 15000 (indexed)", query.plan());
        std::cout << "  results:";
        for (auto& result : std::move(query).stream()) {
            if (!result) {
                std::cerr << result.error().message << '\n';
                return 1;
            }
            std::cout << ' ' << result->name;
        }
        std::cout << '\n';
    }

    // --- Top-2 highest earners. ---
    {
        std::cout << "Top 2 earners:";
        auto query = database->query<EmployeeV3>().top_k(
            2, [](const EmployeeV3& a, const EmployeeV3& b) { return a.salary < b.salary; });
        for (auto& result : std::move(query).stream()) {
            if (!result) {
                std::cerr << result.error().message << '\n';
                return 1;
            }
            std::cout << ' ' << result->name << '(' << result->salary << ')';
        }
        std::cout << '\n';
    }

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}
