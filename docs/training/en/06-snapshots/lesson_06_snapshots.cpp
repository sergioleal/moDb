// Lesson 6 -- Consistent Reports with Snapshots.
// Builds on Lesson 5 (lesson_05_relationships.cpp) -- opens the SAME
// database file; the schema is unchanged.
// See docs/training/en/06-snapshots/06-snapshots.md.

#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

using modb::object::AttributeValue;
using modb::object::BlobId;
using modb::object::FieldId;
using modb::object::ObjectId;
using modb::object::OwnedRef;
using modb::object::Ref;
using modb::object::Snapshot;

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

constexpr FieldId kNameField{1};

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

modb::Result<ObjectId> find_employee_id(modb::object::Database& database, std::string_view name) {
    auto matches =
        database.indexed_object_ids<EmployeeV3>(kNameField, AttributeValue{std::string{name}});
    if (!matches) {
        return std::unexpected(matches.error());
    }
    if (matches->empty()) {
        return std::unexpected(
            modb::Error{modb::ErrorCode::record_not_found, "no employee named " + std::string{name}});
    }
    return (*matches)[0];
}

// Sums every EmployeeV3's salary visible at the given snapshot's epoch.
modb::Result<std::int64_t> payroll_total(modb::object::Database& database, const Snapshot& snapshot) {
    std::int64_t total = 0;
    auto scanned = database.scan<EmployeeV3>(
        snapshot, [&](const EmployeeV3& employee) -> modb::Result<void> {
            total += employee.salary;
            return {};
        });
    if (!scanned) {
        return std::unexpected(scanned.error());
    }
    return total;
}

} // namespace

int main() {
    std::cout << "Objective: a consistent payroll report, a snapshot_conflict, and manual GC.\n";
    const auto path = db_path();

    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << opened.error().message
                  << " -- have you run Lessons 1-5 first? Expected a database at " << path.string()
                  << '\n';
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached || !database->bind(employee_v3_binding()) || !database->bind(department_binding()) ||
        !database->bind(emergency_contact_binding()) || !database->bind(project_binding())) {
        std::cerr << "failed to bind Lesson 6 types\n";
        return 1;
    }

    auto carla_id = find_employee_id(*database, "Carla");
    if (!carla_id) {
        std::cerr << carla_id.error().message << '\n';
        return 1;
    }

    {
        auto snapshot = database->snapshot();
        if (!snapshot) {
            std::cerr << snapshot.error().message << '\n';
            return 1;
        }
        auto before = payroll_total(*database, *snapshot);
        if (!before) {
            std::cerr << before.error().message << '\n';
            return 1;
        }
        std::cout << "Payroll total at the snapshot's epoch = " << *before << '\n';

        // Meanwhile: a raise to Carla, committed while the snapshot above is
        // still open.
        {
            auto tx = database->begin();
            if (!tx) {
                std::cerr << tx.error().message << '\n';
                return 1;
            }
            auto carla_handle = database->get<EmployeeV3>(*carla_id);
            if (!carla_handle) {
                std::cerr << carla_handle.error().message << '\n';
                return 1;
            }
            if (auto updated = carla_handle->set<&EmployeeV3::salary>(*tx, 18000); !updated) {
                std::cerr << updated.error().message << '\n';
                return 1;
            }
            if (!tx->commit()) {
                std::cerr << "failed to commit Carla's raise\n";
                return 1;
            }
        }

        auto still_before = payroll_total(*database, *snapshot);
        if (!still_before) {
            std::cerr << still_before.error().message << '\n';
            return 1;
        }
        std::cout << "Payroll total at the SAME snapshot after Carla's raise = " << *still_before
                  << " (unchanged -- the snapshot doesn't see it)\n";

        // A second write to Carla while the snapshot is still open conflicts.
        {
            auto tx = database->begin();
            if (!tx) {
                std::cerr << tx.error().message << '\n';
                return 1;
            }
            auto carla_handle = database->get<EmployeeV3>(*carla_id);
            if (!carla_handle) {
                std::cerr << carla_handle.error().message << '\n';
                return 1;
            }
            auto conflicted = carla_handle->set<&EmployeeV3::salary>(*tx, 20000);
            if (conflicted) {
                std::cerr << "expected a snapshot_conflict, but the write succeeded\n";
                return 1;
            }
            std::cout << "A second raise while the snapshot is open failed as expected: "
                      << conflicted.error().message << '\n';
            // `tx` rolls back here, at scope exit.
        }
        // `snapshot` closes at the end of this block.
    }

    // Retry now that the blocking snapshot has closed.
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto carla_handle = database->get<EmployeeV3>(*carla_id);
        if (!carla_handle) {
            std::cerr << carla_handle.error().message << '\n';
            return 1;
        }
        if (auto updated = carla_handle->set<&EmployeeV3::salary>(*tx, 20000); !updated) {
            std::cerr << updated.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit the retried raise\n";
            return 1;
        }
    }
    std::cout << "Retried raise for Carla succeeded once the snapshot closed\n";

    auto fresh_snapshot = database->snapshot();
    if (!fresh_snapshot) {
        std::cerr << fresh_snapshot.error().message << '\n';
        return 1;
    }
    auto after = payroll_total(*database, *fresh_snapshot);
    if (!after) {
        std::cerr << after.error().message << '\n';
        return 1;
    }
    std::cout << "Payroll total on a fresh snapshot = " << *after << '\n';

    auto reclaimed = database->collect_garbage();
    if (!reclaimed) {
        std::cerr << reclaimed.error().message << '\n';
        return 1;
    }
    std::cout << "collect_garbage() reclaimed " << *reclaimed << " record(s)\n";

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}
