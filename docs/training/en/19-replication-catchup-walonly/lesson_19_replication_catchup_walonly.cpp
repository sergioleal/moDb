// Lesson 19 -- Replication Catch-up and wal_only.
// Builds on the read-replica lesson by naming the operational commands used
// after bootstrap, including the wal_only branch.
// See docs/training/en/19-replication-catchup-walonly/19-replication-catchup-walonly.md.

#include "modb/object/database.hpp"

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>

namespace {

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

void print_uuid(modb::object::DatabaseUuid uuid) {
    std::cout << std::hex << std::setfill('0');
    for (const std::uint8_t byte : uuid.bytes) {
        std::cout << std::setw(2) << static_cast<unsigned int>(byte);
    }
    std::cout << std::dec << std::setfill(' ');
}

} // namespace

int main() {
    std::cout << "Objective: plan replication catch-up and wal_only operations.\n";
    const auto primary = db_path();
    auto opened = modb::object::Database::open(primary);
    if (!opened) {
        std::cerr << opened.error().message << " -- have you run Lessons 1-18 first?\n";
        return 1;
    }

    const auto wal = primary.string() + ".wal";
    const auto follower = primary.parent_path() / "employee-directory-reporting.modb";

    std::cout << "Primary storage: "
              << (opened->primary_storage() == modb::object::PrimaryStorage::wal_only ? "wal_only"
                                                                                       : "full")
              << '\n';
    std::cout << "Primary uuid: ";
    print_uuid(opened->database_uuid());
    std::cout << '\n';
    std::cout << "Primary timeline: " << opened->timeline_id().value << '\n';
    std::cout << "WAL path: " << wal << '\n';
    std::cout << "WAL present now: " << (std::filesystem::exists(wal) ? "yes" : "no") << '\n';

    std::cout << "CLI catch-up shape:\n";
    std::cout << "  modb replicate catch-up " << follower.string() << ' ' << wal << ' '
              << primary.string() << '\n';
    std::cout << "CLI status shape:\n";
    std::cout << "  modb replicate status " << primary.string() << '\n';
    std::cout << "CLI wal_only seed shape:\n";
    std::cout << "  modb replicate seed-wal " << follower.string() << ' ' << wal << ' '
              << primary.string() << '\n';

    return 0;
}
