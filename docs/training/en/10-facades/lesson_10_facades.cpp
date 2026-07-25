// Lesson 10 -- Facades: A Stable HR API.
// Builds on Lesson 9 (lesson_09_remote_operations.cpp) -- opens the SAME
// database file; the schema is unchanged.
// See docs/training/en/10-facades/10-facades.md.

#include "modb/app/server_connection.hpp"
#include "modb/net/server.hpp"
#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/execution_context.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/facade_handle.hpp"
#include "modb/ops/module_manifest.hpp"
#include "modb/ops/object_access.hpp"
#include "modb/ops/operation.hpp"
#include "modb/ops/operation_registry.hpp"
#include "modb/storage/binary.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

using modb::object::AttributeValue;
using modb::object::BlobId;
using modb::object::FieldId;
using modb::object::ObjectId;
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

modb::Result<ObjectId> find_department_id(modb::object::Database& database, std::string_view name) {
    auto matches = database.indexed_object_ids<Department>(kDepartmentNameField,
                                                           AttributeValue{std::string{name}});
    if (!matches) {
        return std::unexpected(matches.error());
    }
    if (matches->empty()) {
        return std::unexpected(modb::Error{modb::ErrorCode::record_not_found,
                                           "no department named " + std::string{name}});
    }
    return (*matches)[0];
}

// Lesson 9's operation, unchanged: validates the target department exists
// BEFORE mutating the employee.
class TransferDepartment final : public modb::ops::Operation {
public:
    static constexpr std::string_view k_id = "department.transfer";
    static constexpr modb::ops::OperationMode k_mode = modb::ops::OperationMode::read_write;

    TransferDepartment(ObjectId employee_id, ObjectId department_id)
        : employee_id_{employee_id}, department_id_{department_id} {}

    [[nodiscard]] std::string_view id() const noexcept override { return k_id; }
    [[nodiscard]] modb::ops::OperationMode mode() const noexcept override { return k_mode; }

    [[nodiscard]] static modb::Result<std::vector<std::byte>> encode_args(ObjectId employee_id,
                                                                          ObjectId department_id) {
        modb::storage::BinaryWriter writer;
        writer.write_u64(employee_id.value);
        writer.write_u64(department_id.value);
        return std::move(writer).take();
    }

    [[nodiscard]] static modb::Result<std::unique_ptr<Operation>> decode(
        std::span<const std::byte> args) {
        modb::storage::BinaryReader reader{args};
        auto employee_id = reader.read_u64();
        if (!employee_id) {
            return std::unexpected(employee_id.error());
        }
        auto department_id = reader.read_u64();
        if (!department_id) {
            return std::unexpected(department_id.error());
        }
        if (!reader.at_end()) {
            return std::unexpected(
                modb::Error{modb::ErrorCode::invalid_argument, "TransferDepartment args have trailing bytes"});
        }
        return std::unique_ptr<Operation>{
            new TransferDepartment{ObjectId{*employee_id}, ObjectId{*department_id}}};
    }

    [[nodiscard]] modb::Result<modb::ops::OperationResult> execute(
        modb::ops::ExecutionContext& context) override {
        auto& access = context.objects();
        auto department = access.get<Department>(department_id_);
        if (!department) {
            return std::unexpected(
                modb::Error{modb::ErrorCode::invalid_argument, "target department does not exist"});
        }
        auto employee_handle = access.get<EmployeeV3>(employee_id_);
        if (!employee_handle) {
            return std::unexpected(employee_handle.error());
        }
        auto employee_value = access.materialize(*employee_handle);
        if (!employee_value) {
            return std::unexpected(employee_value.error());
        }
        employee_value->department = Ref<Department>{department_id_};
        if (auto updated = access.update(*employee_handle, *employee_value); !updated) {
            return std::unexpected(updated.error());
        }
        return modb::ops::OperationResult{};
    }

private:
    ObjectId employee_id_;
    ObjectId department_id_;
};

// A second operation, exposed alongside TransferDepartment through one
// versioned facade.
class GiveRaise final : public modb::ops::Operation {
public:
    static constexpr std::string_view k_id = "employee.give_raise";
    static constexpr modb::ops::OperationMode k_mode = modb::ops::OperationMode::read_write;

    GiveRaise(ObjectId employee_id, std::int64_t new_salary)
        : employee_id_{employee_id}, new_salary_{new_salary} {}

    [[nodiscard]] std::string_view id() const noexcept override { return k_id; }
    [[nodiscard]] modb::ops::OperationMode mode() const noexcept override { return k_mode; }

    [[nodiscard]] static modb::Result<std::vector<std::byte>> encode_args(ObjectId employee_id,
                                                                          std::int64_t new_salary) {
        modb::storage::BinaryWriter writer;
        writer.write_u64(employee_id.value);
        writer.write_u64(static_cast<std::uint64_t>(new_salary));
        return std::move(writer).take();
    }

    [[nodiscard]] static modb::Result<std::unique_ptr<Operation>> decode(
        std::span<const std::byte> args) {
        modb::storage::BinaryReader reader{args};
        auto employee_id = reader.read_u64();
        if (!employee_id) {
            return std::unexpected(employee_id.error());
        }
        auto new_salary = reader.read_u64();
        if (!new_salary) {
            return std::unexpected(new_salary.error());
        }
        if (!reader.at_end()) {
            return std::unexpected(
                modb::Error{modb::ErrorCode::invalid_argument, "GiveRaise args have trailing bytes"});
        }
        return std::unique_ptr<Operation>{
            new GiveRaise{ObjectId{*employee_id}, static_cast<std::int64_t>(*new_salary)}};
    }

    [[nodiscard]] modb::Result<modb::ops::OperationResult> execute(
        modb::ops::ExecutionContext& context) override {
        if (new_salary_ <= 0) {
            return std::unexpected(
                modb::Error{modb::ErrorCode::invalid_argument, "new salary must be positive"});
        }
        auto& access = context.objects();
        auto handle = access.get<EmployeeV3>(employee_id_);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        auto value = access.materialize(*handle);
        if (!value) {
            return std::unexpected(value.error());
        }
        value->salary = new_salary_;
        if (auto updated = access.update(*handle, *value); !updated) {
            return std::unexpected(updated.error());
        }
        return modb::ops::OperationResult{};
    }

private:
    ObjectId employee_id_;
    std::int64_t new_salary_;
};

// The facade tag the client is compiled against -- version 1, matching what
// the server actually registers below.
struct HRFacade {
    static constexpr std::string_view k_id = "hr";
    static constexpr std::uint32_t k_version = 1;
};

// A second tag, deliberately asking for a version the server never
// registers -- used only to demonstrate the version-mismatch failure.
struct HRFacadeV2 {
    static constexpr std::string_view k_id = "hr";
    static constexpr std::uint32_t k_version = 2;
};

modb::ops::FacadeDescriptor hr_facade_descriptor() {
    return modb::ops::FacadeDescriptor{
        .facade_id = std::string{HRFacade::k_id},
        .facade_version = HRFacade::k_version,
        .mode = modb::ops::FacadeMode::read_write,
        .methods =
            {
                modb::ops::MethodDescriptor{.operation_id = std::string{TransferDepartment::k_id},
                                            .method_version = 1,
                                            .mode = TransferDepartment::k_mode},
                modb::ops::MethodDescriptor{.operation_id = std::string{GiveRaise::k_id},
                                            .method_version = 1,
                                            .mode = GiveRaise::k_mode},
            },
    };
}

modb::ops::ModuleManifest employee_directory_manifest(modb::object::BaselineId baseline) {
    modb::ops::ModuleManifest manifest{
        .id = "employee_directory",
        .module_version = 1,
        .baseline = baseline,
        .api_version = modb::ops::runtime_api_version,
        .methods = {modb::ops::ExportedMethod{.id = std::string{TransferDepartment::k_id},
                                              .mode = TransferDepartment::k_mode},
                    modb::ops::ExportedMethod{.id = std::string{GiveRaise::k_id},
                                              .mode = GiveRaise::k_mode}},
        .facades = {hr_facade_descriptor()},
    };
    manifest.hash = modb::ops::compute_manifest_hash(manifest);
    return manifest;
}

modb::Result<void> register_employee_directory_module(modb::ops::OperationRegistry& registry) {
    if (auto status =
            registry.register_operation<TransferDepartment>(std::string{TransferDepartment::k_id});
        !status) {
        return status;
    }
    return registry.register_operation<GiveRaise>(std::string{GiveRaise::k_id});
}

} // namespace

int main() {
    std::cout << "Objective: expose a stable \"hr\" facade instead of raw operation ids.\n";
    const auto path = db_path();

    auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
    if (!server) {
        std::cerr << server.error().message
                  << " -- have you run Lessons 1-9 first? Expected a database at " << path.string()
                  << '\n';
        return 1;
    }
    if (!server->database().bind(employee_v3_binding()) ||
        !server->database().bind(department_binding()) ||
        !server->database().bind(emergency_contact_binding()) ||
        !server->database().bind(project_binding())) {
        std::cerr << "failed to bind Lesson 10 types on the server\n";
        return 1;
    }

    auto bruno_id = find_employee_id(server->database(), "Bruno");
    auto ana_id = find_employee_id(server->database(), "Ana");
    auto engineering_id = find_department_id(server->database(), "Engineering");
    if (!bruno_id || !ana_id || !engineering_id) {
        std::cerr << "failed to look up Bruno/Ana/Engineering\n";
        return 1;
    }

    auto registry = std::make_shared<modb::ops::OperationRegistry>();
    auto catalog = std::make_shared<modb::ops::FacadeCatalog>();
    modb::ops::ModuleLoader loader;
    const auto& baseline = server->database().current_baseline();
    if (!baseline) {
        std::cerr << "server has no current baseline\n";
        return 1;
    }
    const auto manifest = employee_directory_manifest(baseline->id());
    loader.admit_hash(manifest.hash);
    if (auto loaded = loader.load(manifest, baseline->id(), *registry, *catalog,
                                  register_employee_directory_module);
        !loaded) {
        std::cerr << loaded.error().message << '\n';
        return 1;
    }
    server->set_operation_registry(registry);
    server->set_facade_catalog(catalog);

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

    std::cout << "Opening the \"hr\" facade (version 1)\n";
    auto hr = connection->open_facade<HRFacade>();
    if (!hr) {
        std::cerr << hr.error().message << '\n';
        acceptor.join();
        return 1;
    }

    std::cout << "Moving Bruno back to Engineering through HRFacade::TransferDepartment\n";
    auto transfer = hr->invoke<TransferDepartment>(*bruno_id, *engineering_id);
    if (!transfer) {
        std::cerr << "unexpected failure: " << transfer.error().message << '\n';
        acceptor.join();
        return 1;
    }
    std::cout << "  succeeded\n";

    std::cout << "Giving Ana a raise through HRFacade::GiveRaise\n";
    auto raise = hr->invoke<GiveRaise>(*ana_id, 14500);
    if (!raise) {
        std::cerr << "unexpected failure: " << raise.error().message << '\n';
        acceptor.join();
        return 1;
    }
    std::cout << "  succeeded\n";

    std::cout << "A client compiled against \"hr\" version 2 asks for a version the server "
                 "never published\n";
    auto stale = connection->open_facade<HRFacadeV2>();
    if (stale) {
        std::cerr << "expected the version-2 facade lookup to fail, but it succeeded\n";
        acceptor.join();
        return 1;
    }
    std::cout << "  rejected as expected: " << stale.error().message << '\n';

    acceptor.join();
    return 0;
}
