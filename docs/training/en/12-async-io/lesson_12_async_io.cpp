// Lesson 12 -- Async I/O: Timing WAL Writes.
// Builds on Lesson 11 (lesson_11_graphs.cpp) -- opens the SAME database
// file; the schema is unchanged.
// See docs/training/en/12-async-io/12-async-io.md.

#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"

#include <chrono>
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

struct EmployeeV4 {
    std::string name;
    std::int64_t salary{};
    std::string country;
    Ref<Department> department{};
    OwnedRef<EmergencyContact> emergency_contact{};
    BlobId projects{};
    Ref<EmployeeV4> manager{};
};

modb::object::BindingBuilder<EmployeeV4> employee_v4_binding() {
    modb::object::BindingBuilder<EmployeeV4> builder{"Employee"};
    builder.field<1>("name", &EmployeeV4::name)
        .field<2>("salary", &EmployeeV4::salary)
        .field<3>("country", &EmployeeV4::country, "BR")
        .field<4>("department", &EmployeeV4::department, Ref<Department>{})
        .field<5>("emergency_contact", &EmployeeV4::emergency_contact, OwnedRef<EmergencyContact>{})
        .field<6>("projects", &EmployeeV4::projects, BlobId{})
        .field<7>("manager", &EmployeeV4::manager, Ref<EmployeeV4>{});
    return builder;
}

constexpr FieldId kNameField{1};

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

modb::Result<ObjectId> find_employee_id(modb::object::Database& database, std::string_view name) {
    auto matches =
        database.indexed_object_ids<EmployeeV4>(kNameField, AttributeValue{std::string{name}});
    if (!matches) {
        return std::unexpected(matches.error());
    }
    if (matches->empty()) {
        return std::unexpected(
            modb::Error{modb::ErrorCode::record_not_found, "no employee named " + std::string{name}});
    }
    return (*matches)[0];
}

// Opens the SAME file under the given wal_io mode, times `iterations`
// committed raises to Carla, and reports the elapsed milliseconds.
double raise_loop_ms(const std::filesystem::path& path, const modb::object::DatabaseOptions& options,
                     int iterations, modb::Result<void>& outcome) {
    auto opened = modb::object::Database::open(path, options);
    if (!opened) {
        outcome = std::unexpected(opened.error());
        return 0.0;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached) {
        outcome = std::unexpected(modb::Error{modb::ErrorCode::invalid_argument,
                                              "failed to attach database"});
        return 0.0;
    }
    if (!database->bind(employee_v4_binding()) || !database->bind(department_binding()) ||
        !database->bind(emergency_contact_binding()) || !database->bind(project_binding())) {
        outcome = std::unexpected(
            modb::Error{modb::ErrorCode::invalid_argument, "failed to bind Lesson 12 types"});
        return 0.0;
    }
    auto carla_id = find_employee_id(*database, "Carla");
    if (!carla_id) {
        outcome = std::unexpected(carla_id.error());
        return 0.0;
    }
    auto handle = database->get<EmployeeV4>(*carla_id);
    if (!handle) {
        outcome = std::unexpected(handle.error());
        return 0.0;
    }

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto tx = database->begin();
        if (!tx) {
            outcome = std::unexpected(tx.error());
            return 0.0;
        }
        if (auto set = handle->set<&EmployeeV4::salary>(*tx, 20000 + (i % 2)); !set) {
            outcome = std::unexpected(set.error());
            return 0.0;
        }
        if (!tx->commit()) {
            outcome = std::unexpected(
                modb::Error{modb::ErrorCode::invalid_argument, "commit failed mid-loop"});
            return 0.0;
        }
    }
    const auto end = std::chrono::steady_clock::now();

    modb::object::DatabaseRegistry::instance().detach(*attached);
    outcome = modb::Result<void>{};
    return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace

int main() {
    std::cout << "Objective: compare sync vs. async WAL I/O for the same workload.\n";
    const auto path = db_path();

    // wal_io is a per-open runtime choice, not a persisted format decision
    // -- we can open the SAME file under either mode.
    constexpr int kIterations = 200;

    modb::Result<void> sync_outcome;
    const auto sync_ms =
        raise_loop_ms(path, modb::object::DatabaseOptions{}, kIterations, sync_outcome);
    if (!sync_outcome) {
        std::cerr << sync_outcome.error().message
                  << " -- have you run Lessons 1-11 first? Expected a database at " << path.string()
                  << '\n';
        return 1;
    }
    std::cout << kIterations << " committed raises under wal_io=sync took " << sync_ms << " ms ("
              << (1000.0 * kIterations / sync_ms) << " commits/s)\n";

    modb::Result<void> async_outcome;
    const auto async_ms = raise_loop_ms(
        path, modb::object::DatabaseOptions{.wal_io = modb::object::WalIoMode::async}, kIterations,
        async_outcome);
    if (!async_outcome) {
        std::cerr << async_outcome.error().message << '\n';
        return 1;
    }
    std::cout << kIterations << " committed raises under wal_io=async took " << async_ms << " ms ("
              << (1000.0 * kIterations / async_ms) << " commits/s)\n";

    // Report honestly -- which mode wins depends on the disk, the OS, and
    // how many commits are batched together, so we print the real numbers
    // rather than asserting a winner.
    if (async_ms < sync_ms) {
        std::cout << "Async was faster on this run (" << (sync_ms / async_ms) << "x)\n";
    } else {
        std::cout << "Sync was faster (or tied) on this run (" << (async_ms / sync_ms) << "x)\n";
    }

    // Leave Carla's salary at a clean, meaningful value for later lessons.
    {
        auto opened = modb::object::Database::open(path);
        if (!opened) {
            std::cerr << opened.error().message << '\n';
            return 1;
        }
        auto database = std::make_shared<modb::object::Database>(std::move(*opened));
        auto attached = modb::object::DatabaseRegistry::instance().attach(database);
        if (!attached || !database->bind(employee_v4_binding())) {
            std::cerr << "failed to bind Employee\n";
            return 1;
        }
        auto carla_id = find_employee_id(*database, "Carla");
        if (!carla_id) {
            std::cerr << carla_id.error().message << '\n';
            return 1;
        }
        auto handle = database->get<EmployeeV4>(*carla_id);
        if (!handle) {
            std::cerr << handle.error().message << '\n';
            return 1;
        }
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        if (auto set = handle->set<&EmployeeV4::salary>(*tx, 20000); !set) {
            std::cerr << set.error().message << '\n';
            return 1;
        }
        if (!tx->commit()) {
            std::cerr << "failed to commit Carla's final salary\n";
            return 1;
        }
        modb::object::DatabaseRegistry::instance().detach(*attached);
    }
    std::cout << "Settled Carla's salary back to 20000 for later lessons\n";

    return 0;
}
