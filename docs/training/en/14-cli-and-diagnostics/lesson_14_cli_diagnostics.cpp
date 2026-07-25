// Lesson 14 -- CLI and Diagnostics.
// Builds on Lesson 13 -- opens the SAME database file and inspects it the
// way the CLI's operational commands do.
// See docs/training/en/14-cli-and-diagnostics/14-cli-and-diagnostics.md.

#include "modb/object/database.hpp"
#include "modb/storage/database_check.hpp"

#include <filesystem>
#include <iostream>

namespace {

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

struct Inventory {
    std::size_t slotted{};
    std::size_t blob{};
    std::size_t index{};
    std::size_t catalog{};
    std::size_t unknown{};
    std::size_t errors{};
};

Inventory summarize(const modb::storage::DatabaseCheckReport& report) {
    Inventory out;
    for (const auto& page : report.pages) {
        if (page.error) {
            ++out.errors;
        }
        switch (page.kind) {
        case modb::storage::PageKind::slotted:
        case modb::storage::PageKind::table_heap_root:
            ++out.slotted;
            break;
        case modb::storage::PageKind::blob:
            ++out.blob;
            break;
        case modb::storage::PageKind::index_directory:
        case modb::storage::PageKind::btree_leaf:
        case modb::storage::PageKind::btree_internal:
            ++out.index;
            break;
        case modb::storage::PageKind::database_root:
        case modb::storage::PageKind::identity_directory:
        case modb::storage::PageKind::identity_entries:
            ++out.catalog;
            break;
        case modb::storage::PageKind::unknown:
            ++out.unknown;
            break;
        default:
            break;
        }
    }
    out.errors += report.heap_errors.size() + report.record_errors.size();
    return out;
}

} // namespace

int main() {
    std::cout << "Objective: inspect the training database the way an operator would.\n";
    const auto path = db_path();
    std::cout << "Database file: " << path.string() << '\n';

    auto checked = modb::storage::check_database(path);
    if (!checked) {
        std::cerr << checked.error().message << '\n';
        return 1;
    }
    const auto inventory = summarize(*checked);
    std::cout << "Page inventory: total=" << checked->page_count << '\n';
    std::cout << "  slotted=" << inventory.slotted << " blob=" << inventory.blob
              << " index=" << inventory.index << " catalog=" << inventory.catalog << '\n';
    std::cout << "  unknown=" << inventory.unknown << " errors=" << inventory.errors << '\n';
    std::cout << "Database check: " << (checked->ok() ? "OK" : "FAILED") << '\n';

    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << opened.error().message
                  << " -- have you run Lessons 1-13 first? Expected a database at "
                  << path.string() << '\n';
        return 1;
    }

    if (const auto& baseline = opened->current_baseline()) {
        std::cout << "Current baseline: " << baseline->id().value << " with "
                  << baseline->types().size() << " active type(s)\n";
    } else {
        std::cout << "Current baseline: none\n";
    }

    const auto wal_path = path.string() + ".wal";
    std::cout << "WAL present: " << (std::filesystem::exists(wal_path) ? "yes" : "no") << '\n';

    std::cout << "Useful CLI follow-ups:\n";
    std::cout << "  modb db check " << path.string() << '\n';
    std::cout << "  modb tx wal-info " << path.string() << '\n';
    std::cout << "  modb baseline list " << path.string() << '\n';

    return checked->ok() ? 0 : 1;
}
