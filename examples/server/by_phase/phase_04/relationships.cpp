#include "modb/object/database.hpp"
#include "modb/object/ref.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace {

struct Department {
    std::string name;
};

struct Badge {
    std::int64_t code{};
};

struct Employee {
    std::string name;
    modb::object::Ref<Department> department{};
    modb::object::OwnedRef<Badge> badge{};
};

modb::object::BindingBuilder<Department> department_binding() {
    modb::object::BindingBuilder<Department> builder{"Department"};
    builder.field<1>("name", &Department::name);
    return builder;
}

modb::object::BindingBuilder<Badge> badge_binding() {
    modb::object::BindingBuilder<Badge> builder{"Badge"};
    builder.field<1>("code", &Badge::code);
    return builder;
}

modb::object::BindingBuilder<Employee> employee_binding() {
    modb::object::BindingBuilder<Employee> builder{"Employee"};
    builder.field<1>("name", &Employee::name)
        .field<2>("department", &Employee::department)
        .field<3>("badge", &Employee::badge);
    return builder;
}

std::filesystem::path temp_path() {
    return std::filesystem::temp_directory_path() /
           ("ring0-phase-04-" +
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
    if (!database->bind(department_binding()) || !database->bind(badge_binding()) ||
        !database->bind(employee_binding())) {
        std::cerr << "failed to bind relationship types\n";
        cleanup(path);
        return 1;
    }

    auto tx = database->begin();
    auto department = database->create(*tx, Department{"Engineering"});
    auto badge = database->create(*tx, Badge{7});
    auto employee = database->create(*tx, Employee{"Ana", {department->id()}, {badge->id()}});
    if (!department || !badge || !employee || !tx->commit()) {
        std::cerr << "failed to store relationship graph\n";
        cleanup(path);
        return 1;
    }

    auto ana = database->materialize(*database->get<Employee>(employee->id()));
    std::cout << ana->name << " department_ref=" << ana->department.target.value
              << " badge_owned_ref=" << ana->badge.target.value << '\n';
    modb::object::DatabaseRegistry::instance().detach(*attached);
    cleanup(path);
    return 0;
}
