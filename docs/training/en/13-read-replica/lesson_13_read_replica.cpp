// Lesson 13 -- Read Replica: Bootstrapping a Follower.
// Builds on Lesson 12 (lesson_12_async_io.cpp) -- opens the SAME database
// file as the primary; the schema is unchanged. This is the last lesson,
// so the follower it creates is this lesson's own throwaway artifact, not
// part of the chain any later lesson continues from.
// See docs/training/en/13-read-replica/13-read-replica.md.

#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"
#include "modb/repl/replication.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

using modb::object::BlobId;
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

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

// Sums every EmployeeV4's salary visible at the given snapshot's epoch.
modb::Result<std::int64_t> payroll_total(modb::object::Database& database, const Snapshot& snapshot) {
    std::int64_t total = 0;
    auto scanned = database.scan<EmployeeV4>(
        snapshot, [&](const EmployeeV4& employee) -> modb::Result<void> {
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
    std::cout << "Objective: bootstrap a read-only replica and query it independently.\n";
    const auto path = db_path();

    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << opened.error().message
                  << " -- have you run Lessons 1-12 first? Expected a database at " << path.string()
                  << '\n';
        return 1;
    }
    auto primary = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(primary);
    if (!attached || !primary->bind(employee_v4_binding()) || !primary->bind(department_binding()) ||
        !primary->bind(emergency_contact_binding()) || !primary->bind(project_binding())) {
        std::cerr << "failed to bind Lesson 13 types on the primary\n";
        return 1;
    }

    // The follower is a throwaway artifact of this one run, next to the
    // main file but not part of the lesson-to-lesson chain.
    const auto temp_dir = path.parent_path() / "employee-directory-bootstrap-tmp";
    const auto follower_path = path.parent_path() / "employee-directory-replica.modb";
    std::error_code ignored;
    std::filesystem::remove(follower_path, ignored);
    std::filesystem::remove(follower_path.string() + ".wal", ignored);

    // create_bootstrap_snapshot briefly opens a barrier transaction on the
    // primary to copy a consistent data file (see modb replicate bootstrap).
    auto snap = modb::repl::create_bootstrap_snapshot(*primary, temp_dir);
    if (!snap) {
        std::cerr << snap.error().message << '\n';
        return 1;
    }
    std::cout << "Bootstrap snapshot cut at LSN " << snap->begin.cut_lsn << " (" << snap->begin.size_bytes
              << " bytes)\n";
    if (auto installed = modb::repl::install_bootstrap_snapshot(*snap, follower_path); !installed) {
        std::cerr << installed.error().message << '\n';
        return 1;
    }
    std::cout << "Installed the follower copy at " << follower_path.string() << '\n';

    auto follower_opened = modb::object::Database::open(follower_path);
    if (!follower_opened) {
        std::cerr << follower_opened.error().message << '\n';
        return 1;
    }
    auto follower = std::make_shared<modb::object::Database>(std::move(*follower_opened));
    auto follower_attached = modb::object::DatabaseRegistry::instance().attach(follower);
    if (!follower_attached) {
        std::cerr << "failed to attach follower\n";
        return 1;
    }
    if (!follower->bind(employee_v4_binding()) || !follower->bind(department_binding()) ||
        !follower->bind(emergency_contact_binding()) || !follower->bind(project_binding())) {
        std::cerr << "failed to bind Lesson 13 types on the follower\n";
        return 1;
    }
    if (follower->database_uuid() != primary->database_uuid()) {
        std::cerr << "follower UUID does not match the primary\n";
        return 1;
    }
    std::cout << "Follower's database_uuid matches the primary's\n";
    (void)follower->set_read_only_replica(true);

    // A Lesson-6-style payroll report -- now against the follower.
    auto follower_snapshot = follower->snapshot();
    if (!follower_snapshot) {
        std::cerr << follower_snapshot.error().message << '\n';
        return 1;
    }
    auto total = payroll_total(*follower, *follower_snapshot);
    if (!total) {
        std::cerr << total.error().message << '\n';
        return 1;
    }
    std::cout << "Payroll total from the follower = " << *total << '\n';

    // A read-only replica refuses to even open a write transaction.
    auto denied = follower->begin();
    if (denied) {
        std::cerr << "expected the follower to reject writes, but begin() succeeded\n";
        return 1;
    }
    if (denied.error().code != modb::ErrorCode::replica_read_only) {
        std::cerr << "expected replica_read_only, got: " << denied.error().message << '\n';
        return 1;
    }
    std::cout << "Follower rejected a write attempt as expected: " << denied.error().message << '\n';

    modb::object::DatabaseRegistry::instance().detach(*follower_attached);
    modb::object::DatabaseRegistry::instance().detach(*attached);
    // detach() only drops the registry's reference -- these shared_ptrs
    // still hold the Database (and its open file handle) alive until
    // reset, and Windows refuses to delete a file with an open handle.
    follower.reset();
    primary.reset();

    std::filesystem::remove(follower_path, ignored);
    std::filesystem::remove(follower_path.string() + ".wal", ignored);
    std::filesystem::remove_all(temp_dir, ignored);

    return 0;
}
