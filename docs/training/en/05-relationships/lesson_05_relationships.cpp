// Lesson 5 -- Relationships: Departments and Projects.
// Builds on Lesson 4 (lesson_04_handles.cpp) -- opens the SAME database
// file and adds new relationship fields to Employee.
// See docs/training/en/05-relationships/05-relationships.md.

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
using modb::object::PersistentVector;
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

// The Lesson 4 shape plus three relationship fields, each zero-valued by
// default so Ana/Bruno/Carla's existing 3-field records still project
// cleanly.
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

constexpr FieldId kEmployeeNameField{1};
constexpr FieldId kDepartmentNameField{1};

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

modb::Result<ObjectId> find_employee_id(modb::object::Database& database, std::string_view name) {
    auto matches = database.indexed_object_ids<EmployeeV3>(kEmployeeNameField,
                                                           AttributeValue{std::string{name}});
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
    std::cout << "Objective: model departments, an owned emergency contact, and a project list.\n";
    const auto path = db_path();

    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << opened.error().message
                  << " -- have you run Lessons 1-4 first? Expected a database at " << path.string()
                  << '\n';
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached || !database->bind(employee_v3_binding()) || !database->bind(department_binding()) ||
        !database->bind(emergency_contact_binding()) || !database->bind(project_binding())) {
        std::cerr << "failed to bind Lesson 5 types\n";
        return 1;
    }

    auto ana_id = find_employee_id(*database, "Ana");
    auto bruno_id = find_employee_id(*database, "Bruno");
    auto carla_id = find_employee_id(*database, "Carla");
    if (!ana_id || !bruno_id || !carla_id) {
        std::cerr << "failed to look up Ana/Bruno/Carla\n";
        return 1;
    }

    ObjectId engineering_id{};
    ObjectId sales_id{};
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto engineering = database->create(*tx, Department{"Engineering"});
        auto sales = database->create(*tx, Department{"Sales"});
        if (!engineering || !sales) {
            std::cerr << "failed to create departments\n";
            return 1;
        }
        engineering_id = engineering->id();
        sales_id = sales->id();
        if (!tx->commit()) {
            std::cerr << "failed to commit departments\n";
            return 1;
        }
        // Lesson 9's remote operation needs to find "Engineering" by name,
        // as a separate program run -- same reasoning as Lesson 2's name
        // index on Employee.
        if (auto created = database->create_index<Department>(kDepartmentNameField); !created) {
            std::cerr << created.error().message << '\n';
            return 1;
        }
    }
    std::cout << "Created departments Engineering=" << engineering_id.value
              << ", Sales=" << sales_id.value << '\n';

    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto ana_handle = database->get<EmployeeV3>(*ana_id);
        auto bruno_handle = database->get<EmployeeV3>(*bruno_id);
        if (!ana_handle || !bruno_handle) {
            std::cerr << "failed to look up Ana/Bruno\n";
            return 1;
        }
        if (auto set = ana_handle->set<&EmployeeV3::department>(*tx, Ref<Department>{engineering_id});
            !set) {
            std::cerr << set.error().message << '\n';
            return 1;
        }
        if (auto set = bruno_handle->set<&EmployeeV3::department>(*tx, Ref<Department>{sales_id});
            !set) {
            std::cerr << set.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit department assignments\n";
            return 1;
        }
    }
    std::cout << "Assigned Ana -> Engineering, Bruno -> Sales\n";

    ObjectId diego_id{};
    ObjectId contact_id{};
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto contact = database->create(*tx, EmergencyContact{"Elena", "+55-11-90000-0000"});
        if (!contact) {
            std::cerr << contact.error().message << '\n';
            return 1;
        }
        contact_id = contact->id();
        auto diego = database->create(
            *tx, EmployeeV3{"Diego", 8000, "BR", Ref<Department>{}, OwnedRef<EmergencyContact>{contact_id},
                            BlobId{}});
        if (!diego) {
            std::cerr << diego.error().message << '\n';
            return 1;
        }
        diego_id = diego->id();
        if (!tx->commit()) {
            std::cerr << "failed to commit Diego and his emergency contact\n";
            return 1;
        }
    }
    std::cout << "Diego=" << diego_id.value << " has emergency contact " << contact_id.value << '\n';
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        if (auto removed = database->remove(*tx, diego_id); !removed) {
            std::cerr << removed.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit Diego's removal\n";
            return 1;
        }
    }
    auto contact_after = database->get<EmergencyContact>(contact_id);
    if (contact_after) {
        std::cout << "After removing Diego, his emergency contact still resolves (unexpected)\n";
    } else {
        std::cout << "After removing Diego, his emergency contact is gone too (cascade-deleted): "
                  << contact_after.error().message << '\n';
    }

    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto phoenix = database->create(*tx, Project{"Phoenix"});
        auto atlas = database->create(*tx, Project{"Atlas"});
        if (!phoenix || !atlas) {
            std::cerr << "failed to create projects\n";
            return 1;
        }
        auto blobs = database->blobs();
        auto projects = PersistentVector<Ref<Project>>::create(blobs, *tx);
        if (!projects) {
            std::cerr << projects.error().message << '\n';
            return 1;
        }
        if (auto pushed = projects->push_back(*tx, Ref<Project>{phoenix->id()}); !pushed) {
            std::cerr << pushed.error().message << '\n';
            return 1;
        }
        if (auto pushed = projects->push_back(*tx, Ref<Project>{atlas->id()}); !pushed) {
            std::cerr << pushed.error().message << '\n';
            return 1;
        }
        auto carla_handle = database->get<EmployeeV3>(*carla_id);
        if (!carla_handle) {
            std::cerr << carla_handle.error().message << '\n';
            return 1;
        }
        if (auto set = carla_handle->set<&EmployeeV3::projects>(*tx, projects->id()); !set) {
            std::cerr << set.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit Carla's projects\n";
            return 1;
        }
    }
    {
        auto carla_handle = database->get<EmployeeV3>(*carla_id);
        if (!carla_handle) {
            std::cerr << carla_handle.error().message << '\n';
            return 1;
        }
        auto carla_value = database->materialize(*carla_handle);
        if (!carla_value) {
            std::cerr << carla_value.error().message << '\n';
            return 1;
        }
        auto blobs = database->blobs();
        PersistentVector<Ref<Project>> projects{blobs, carla_value->projects};
        std::cout << "Carla's projects:";
        auto listed = projects.for_each([&](const Ref<Project>& ref) -> modb::Result<void> {
            auto project = database->get<Project>(ref.target);
            if (!project) {
                return std::unexpected(project.error());
            }
            auto value = database->materialize(*project);
            if (!value) {
                return std::unexpected(value.error());
            }
            std::cout << ' ' << value->name;
            return {};
        });
        if (!listed) {
            std::cerr << listed.error().message << '\n';
            return 1;
        }
        std::cout << '\n';
    }

    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        if (auto removed = database->remove(*tx, sales_id); !removed) {
            std::cerr << removed.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit Sales' removal\n";
            return 1;
        }
    }
    auto sales_after = database->get<Department>(sales_id);
    if (sales_after) {
        std::cout << "After removing Sales, resolving it directly still succeeds (unexpected)\n";
    } else {
        std::cout << "After removing Sales, resolving it directly fails as expected: "
                  << sales_after.error().message
                  << " -- Bruno's `department` field still holds that (now dangling) id\n";
    }

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}
