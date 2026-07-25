// Lesson 8 -- Serving the Directory Over the Network.
// Builds on Lesson 7 (lesson_07_queries.cpp) -- opens the SAME database
// file; the schema is unchanged.
// See docs/training/en/08-networking/08-networking.md.

#include "modb/app/server_connection.hpp"
#include "modb/net/server.hpp"
#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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

const modb::object::AttributeValue* find_field(const modb::object::DecodedObject& object,
                                               FieldId field) {
    for (const auto& [value_field, value] : object.fields) {
        if (value_field == field) {
            return &value;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    std::cout << "Objective: serve the directory and query it remotely with ServerConnection.\n";
    const auto path = db_path();

    auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
    if (!server) {
        std::cerr << server.error().message
                  << " -- have you run Lessons 1-7 first? Expected a database at " << path.string()
                  << '\n';
        return 1;
    }
    if (!server->database().bind(employee_v3_binding()) ||
        !server->database().bind(department_binding()) ||
        !server->database().bind(emergency_contact_binding()) ||
        !server->database().bind(project_binding())) {
        std::cerr << "failed to bind Lesson 8 types on the server\n";
        return 1;
    }
    auto employee_type = server->database().type_id_of<EmployeeV3>();
    if (!employee_type) {
        std::cerr << employee_type.error().message << '\n';
        return 1;
    }

    std::thread acceptor([&server] { (void)server->serve_one(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    auto connection = modb::app::ServerConnection::connect({
        .host = "127.0.0.1",
        .port = server->port(),
        .database_name = std::string{server->database_name()},
    });
    if (!connection) {
        std::cerr << connection.error().message << '\n';
        acceptor.join();
        return 1;
    }
    std::cout << "Handshake ok, protocol " << connection->info().protocol_major << '.'
              << connection->info().protocol_minor
              << ", max_concurrent_streams=" << connection->info().max_concurrent_streams << '\n';

    auto everyone = connection->collect(modb::net::QueryDescription{.type = *employee_type, .limit = 10});
    if (!everyone) {
        std::cerr << everyone.error().message << '\n';
        acceptor.join();
        return 1;
    }
    std::cout << "Remote query returned " << everyone->size() << " employees\n";

    constexpr FieldId salary_field{2};
    constexpr FieldId name_field{1};
    auto exact_match = connection->collect(modb::net::QueryDescription{
        .type = *employee_type,
        .equals = modb::net::EqualityFilter{salary_field, AttributeValue{std::int64_t{20000}}},
    });
    if (!exact_match) {
        std::cerr << exact_match.error().message << '\n';
        acceptor.join();
        return 1;
    }
    std::cout << "Remote search for salary == 20000 returned " << exact_match->size()
              << " match(es):";
    for (const auto& object : *exact_match) {
        if (const auto* name = find_field(object, name_field)) {
            auto as_string = name->as_string();
            if (as_string) {
                std::cout << ' ' << *as_string;
            }
        }
    }
    std::cout << '\n';

    acceptor.join();
    return 0;
}
