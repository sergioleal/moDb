// Lesson 10 -- Facades: A Stable HR API.
// Builds on Lesson 9 (lesson_09_remote_operations.cpp).
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
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

using modb::object::AttributeValue;
using modb::object::BlobId;
using modb::object::FieldId;
using modb::object::ObjectId;
using modb::object::OwnedRef;
using modb::object::PersistentVector;
using modb::object::Ref;
using modb::object::Snapshot;

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

// Lesson 9: business logic that runs on the server, callable by id.
// Validates the target department exists BEFORE mutating the employee.
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
        // Validate the target department exists before touching the employee.
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

// Lesson 10: a second operation, exposed alongside TransferDepartment
// through one versioned facade.
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
// registers -- used only to demonstrate incompatible_facade_version.
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

struct DirectoryIds {
    ObjectId ana{};
    ObjectId bruno{};
    ObjectId carla{};
    ObjectId engineering{};
    ObjectId sales{};
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

int print_salary(modb::object::Database& database, ObjectId id, std::string_view label) {
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

int lesson_05_relationships(const std::filesystem::path& path, DirectoryIds& ids) {
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
    if (!database->bind(employee_v3_binding()) || !database->bind(department_binding()) ||
        !database->bind(emergency_contact_binding()) || !database->bind(project_binding())) {
        std::cerr << "failed to bind Lesson 5 types\n";
        return 1;
    }

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
        ids.engineering = engineering->id();
        ids.sales = sales->id();
        if (!tx->commit()) {
            std::cerr << "failed to commit departments\n";
            return 1;
        }
    }
    std::cout << "Lesson 5: created departments Engineering=" << ids.engineering.value
              << ", Sales=" << ids.sales.value << '\n';
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto ana_handle = database->get<EmployeeV3>(ids.ana);
        auto bruno_handle = database->get<EmployeeV3>(ids.bruno);
        if (!ana_handle || !bruno_handle) {
            std::cerr << "failed to look up Ana/Bruno\n";
            return 1;
        }
        if (auto set = ana_handle->set<&EmployeeV3::department>(*tx, Ref<Department>{ids.engineering});
            !set) {
            std::cerr << set.error().message << '\n';
            return 1;
        }
        if (auto set = bruno_handle->set<&EmployeeV3::department>(*tx, Ref<Department>{ids.sales});
            !set) {
            std::cerr << set.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit department assignments\n";
            return 1;
        }
    }
    std::cout << "Lesson 5: assigned Ana -> Engineering, Bruno -> Sales\n";

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
    std::cout << "Lesson 5: Diego=" << diego_id.value << " has emergency contact "
              << contact_id.value << '\n';
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
        std::cout << "Lesson 5: after removing Diego, his emergency contact still resolves "
                     "(unexpected)\n";
    } else {
        std::cout << "Lesson 5: after removing Diego, his emergency contact is gone too "
                     "(cascade-deleted): "
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
        auto carla_handle = database->get<EmployeeV3>(ids.carla);
        if (!carla_handle) {
            std::cerr << carla_handle.error().message << '\n';
            return 1;
        }
        if (auto set = carla_handle->set<&EmployeeV3::projects>(*tx, projects->id()); !set) {
            std::cerr << set.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit Carla's project assignments\n";
            return 1;
        }
    }
    {
        auto carla_handle = database->get<EmployeeV3>(ids.carla);
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
        std::cout << "Lesson 5: Carla's projects:";
        auto listed = projects.for_each([&](const Ref<Project>& ref) -> modb::Result<void> {
            auto project = database->get<Project>(ref.target);
            if (!project) {
                return std::unexpected(project.error());
            }
            auto project_value = database->materialize(*project);
            if (!project_value) {
                return std::unexpected(project_value.error());
            }
            std::cout << ' ' << project_value->name;
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
        if (auto removed = database->remove(*tx, ids.sales); !removed) {
            std::cerr << removed.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit Sales's removal\n";
            return 1;
        }
    }
    auto sales_after = database->get<Department>(ids.sales);
    if (sales_after) {
        std::cout << "Lesson 5: after removing Sales, resolving it directly still works "
                     "(unexpected)\n";
    } else {
        std::cout << "Lesson 5: after removing Sales, resolving it directly fails as expected: "
                  << sales_after.error().message
                  << " -- Bruno's `department` field still holds that (now dangling) id\n";
    }

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
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

// Lesson 6: a payroll report that stays consistent across a concurrent
// raise, a forced snapshot_conflict + retry, and manual garbage collection.
int lesson_06_snapshots(const std::filesystem::path& path, const DirectoryIds& ids) {
    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << opened.error().message << '\n';
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached || !database->bind(employee_v3_binding()) || !database->bind(department_binding()) ||
        !database->bind(emergency_contact_binding()) || !database->bind(project_binding())) {
        std::cerr << "failed to bind Lesson 6 types\n";
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
        std::cout << "Lesson 6: payroll total at the snapshot's epoch = " << *before << '\n';

        // --- Meanwhile: Carla gets a raise, committed after the snapshot
        //     was already open. ---
        {
            auto tx = database->begin();
            if (!tx) {
                std::cerr << tx.error().message << '\n';
                return 1;
            }
            auto carla_handle = database->get<EmployeeV3>(ids.carla);
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
        std::cout << "Lesson 6: payroll total at the SAME snapshot after Carla's raise = "
                  << *still_before << " (unchanged -- the snapshot doesn't see it)\n";

        // --- A second write to Carla while `snapshot` still needs her
        //     `previous` value fails with snapshot_conflict. ---
        {
            auto tx = database->begin();
            if (!tx) {
                std::cerr << tx.error().message << '\n';
                return 1;
            }
            auto carla_handle = database->get<EmployeeV3>(ids.carla);
            if (!carla_handle) {
                std::cerr << carla_handle.error().message << '\n';
                return 1;
            }
            auto conflicted = carla_handle->set<&EmployeeV3::salary>(*tx, 20000);
            if (conflicted) {
                std::cerr << "expected snapshot_conflict, but the write succeeded\n";
                return 1;
            }
            std::cout << "Lesson 6: second raise while the snapshot is open failed as expected: "
                      << conflicted.error().message << '\n';
            // Let `tx` roll back at scope exit -- nothing was written.
        }
        // `snapshot` is destroyed at the end of this block, freeing its hold
        // on Carla's `previous` value.
    }

    // --- Retry now that the snapshot is gone: it succeeds. ---
    {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto carla_handle = database->get<EmployeeV3>(ids.carla);
        if (!carla_handle) {
            std::cerr << carla_handle.error().message << '\n';
            return 1;
        }
        if (auto updated = carla_handle->set<&EmployeeV3::salary>(*tx, 20000); !updated) {
            std::cerr << "retry failed unexpectedly: " << updated.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit the retried raise\n";
            return 1;
        }
    }
    std::cout << "Lesson 6: retried raise for Carla succeeded once the snapshot closed\n";

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
    std::cout << "Lesson 6: payroll total on a fresh snapshot = " << *after << '\n';

    auto reclaimed = database->collect_garbage();
    if (!reclaimed) {
        std::cerr << reclaimed.error().message << '\n';
        return 1;
    }
    std::cout << "Lesson 6: collect_garbage() reclaimed " << *reclaimed << " record(s)\n";

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}

void print_plan(std::string_view label, const modb::query::QueryPlan& plan) {
    std::cout << "  " << label << ": access=" << plan.access_name()
              << " index_available=" << (plan.index_available ? "true" : "false") << '\n';
}

// Lesson 7: search employees by salary, first as a table scan, then backed
// by an index; a top-k "highest earners" query.
int lesson_07_queries(const std::filesystem::path& path, const DirectoryIds&) {
    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << opened.error().message << '\n';
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
    std::cout << "Lesson 7: created an index on Employee.salary\n";
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
        std::cout << "Lesson 7: top 2 earners:";
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

// Pulls one field's value out of a DecodedObject's generic field list --
// the shape a remote QueryDescription result comes back as, since a
// different process might not share our EmployeeV3 struct.
const AttributeValue* find_field(const modb::object::DecodedObject& object, FieldId field) {
    for (const auto& [id, value] : object.fields) {
        if (id.value == field.value) {
            return &value;
        }
    }
    return nullptr;
}

// Lesson 8: split into a server (owns the file) and a client (connects over
// TCP), both in this one process -- the same convention already used by
// examples/server/by_phase/phase_08 onward.
int lesson_08_networking(const std::filesystem::path& path, const DirectoryIds&) {
    auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
    if (!server) {
        std::cerr << server.error().message << '\n';
        return 1;
    }
    if (!server->database().bind(employee_v3_binding()) ||
        !server->database().bind(department_binding()) ||
        !server->database().bind(emergency_contact_binding()) ||
        !server->database().bind(project_binding())) {
        std::cerr << "failed to bind Lesson 5 types on the server\n";
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
    std::cout << "Lesson 8: handshake ok, protocol " << connection->info().protocol_major << '.'
              << connection->info().protocol_minor
              << ", max_concurrent_streams=" << connection->info().max_concurrent_streams << '\n';

    auto everyone = connection->collect(modb::net::QueryDescription{.type = *employee_type, .limit = 10});
    if (!everyone) {
        std::cerr << everyone.error().message << '\n';
        acceptor.join();
        return 1;
    }
    std::cout << "Lesson 8: remote query returned " << everyone->size() << " employees\n";

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
    std::cout << "Lesson 8: remote search for salary == 20000 returned " << exact_match->size()
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

// Lesson 9: TransferDepartment runs on the server; the client only knows
// its id and argument encoding.
int lesson_09_remote_operations(const std::filesystem::path& path, const DirectoryIds& ids) {
    auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
    if (!server) {
        std::cerr << server.error().message << '\n';
        return 1;
    }
    if (!server->database().bind(employee_v3_binding()) ||
        !server->database().bind(department_binding()) ||
        !server->database().bind(emergency_contact_binding()) ||
        !server->database().bind(project_binding())) {
        std::cerr << "failed to bind Lesson 5 types on the server\n";
        return 1;
    }

    auto registry = std::make_shared<modb::ops::OperationRegistry>();
    modb::ops::ModuleLoader loader;
    const auto& baseline = server->database().current_baseline();
    if (!baseline) {
        std::cerr << "server has no current baseline\n";
        return 1;
    }
    const auto manifest = employee_directory_manifest(baseline->id());
    loader.admit_hash(manifest.hash);
    if (auto loaded = loader.load(manifest, baseline->id(), *registry, register_employee_directory_module);
        !loaded) {
        std::cerr << loaded.error().message << '\n';
        return 1;
    }
    server->set_operation_registry(registry);

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

    std::cout << "Lesson 9: transferring Bruno to Engineering (his old department, Sales, was "
                 "removed back in Lesson 5)\n";
    auto good_args = TransferDepartment::encode_args(ids.bruno, ids.engineering);
    if (!good_args) {
        std::cerr << good_args.error().message << '\n';
        acceptor.join();
        return 1;
    }
    auto good_result = connection->call(TransferDepartment::k_id, *good_args);
    if (!good_result) {
        std::cerr << "unexpected failure: " << good_result.error().message << '\n';
        acceptor.join();
        return 1;
    }
    std::cout << "  succeeded\n";

    std::cout << "Lesson 9: attempting a transfer to a department that doesn't exist\n";
    auto bad_args = TransferDepartment::encode_args(ids.bruno, ObjectId{999999});
    if (!bad_args) {
        std::cerr << bad_args.error().message << '\n';
        acceptor.join();
        return 1;
    }
    auto bad_result = connection->call(TransferDepartment::k_id, *bad_args);
    if (bad_result) {
        std::cerr << "expected the bad transfer to fail, but it succeeded\n";
        acceptor.join();
        return 1;
    }
    std::cout << "  failed as expected: " << bad_result.error().message << '\n';

    acceptor.join();
    return 0;
}

int lesson_10_facades(const std::filesystem::path& path, const DirectoryIds& ids) {
    auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
    if (!server) {
        std::cerr << server.error().message << '\n';
        return 1;
    }
    if (!server->database().bind(employee_v3_binding()) ||
        !server->database().bind(department_binding()) ||
        !server->database().bind(emergency_contact_binding()) ||
        !server->database().bind(project_binding())) {
        std::cerr << "failed to bind Lesson 5 types on the server\n";
        return 1;
    }

    // Unlike Lesson 9, we now build BOTH an OperationRegistry and a
    // FacadeCatalog -- the catalog is what lets a client discover the
    // stable "hr" surface instead of calling operation ids directly.
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

    std::cout << "Lesson 10: opening the \"hr\" facade (version 1)\n";
    auto hr = connection->open_facade<HRFacade>();
    if (!hr) {
        std::cerr << hr.error().message << '\n';
        acceptor.join();
        return 1;
    }

    std::cout << "Lesson 10: moving Bruno back to Engineering through HRFacade::TransferDepartment\n";
    auto transfer = hr->invoke<TransferDepartment>(ids.bruno, ids.engineering);
    if (!transfer) {
        std::cerr << "unexpected failure: " << transfer.error().message << '\n';
        acceptor.join();
        return 1;
    }
    std::cout << "  succeeded\n";

    std::cout << "Lesson 10: giving Ana a raise through HRFacade::GiveRaise\n";
    auto raise = hr->invoke<GiveRaise>(ids.ana, 14500);
    if (!raise) {
        std::cerr << "unexpected failure: " << raise.error().message << '\n';
        acceptor.join();
        return 1;
    }
    std::cout << "  succeeded\n";

    std::cout << "Lesson 10: a client compiled against \"hr\" version 2 asks for a version the "
                 "server never published\n";
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

} // namespace

int main() {
    std::cout << "Objective: expose a stable \"hr\" facade instead of raw operation ids.\n";
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
    if (const auto status = lesson_04_handles(path, ids); status != 0) {
        cleanup(path);
        return status;
    }
    if (const auto status = lesson_05_relationships(path, ids); status != 0) {
        cleanup(path);
        return status;
    }
    if (const auto status = lesson_06_snapshots(path, ids); status != 0) {
        cleanup(path);
        return status;
    }
    if (const auto status = lesson_07_queries(path, ids); status != 0) {
        cleanup(path);
        return status;
    }
    if (const auto status = lesson_08_networking(path, ids); status != 0) {
        cleanup(path);
        return status;
    }
    if (const auto status = lesson_09_remote_operations(path, ids); status != 0) {
        cleanup(path);
        return status;
    }
    const auto status = lesson_10_facades(path, ids);

    cleanup(path);
    return status;
}
