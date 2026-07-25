// Lesson 15 -- Storage Internals.
// Builds on Lesson 14's operational inventory by creating a tiny lab file
// and touching PageFile + SlottedPage directly.
// See docs/training/en/15-storage-internals/15-storage-internals.md.

#include "modb/storage/database_check.hpp"
#include "modb/storage/page_file.hpp"
#include "modb/storage/slotted_page.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

std::filesystem::path lab_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory-storage-lab.modb";
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return out;
}

} // namespace

int main() {
    std::cout << "Objective: inspect the page and slot layer under object storage.\n";
    const auto path = lab_path();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    {
        auto created = modb::storage::PageFile::create(path);
        if (!created) {
            std::cerr << created.error().message << '\n';
            return 1;
        }
        std::cout << "Created storage lab: " << path.string() << '\n';

        auto page_id = created->allocate_page();
        if (!page_id) {
            std::cerr << page_id.error().message << '\n';
            return 1;
        }
        std::cout << "Allocated record page: " << page_id->value << '\n';

        auto page = modb::storage::SlottedPage::create();
        auto ana = page.insert(bytes("Ana | Engineering | 14500"));
        auto bruno = page.insert(bytes("Bruno | Sales | 11250"));
        if (!ana || !bruno) {
            std::cerr << "failed to insert lab records\n";
            return 1;
        }
        std::cout << "Inserted two records: slots " << ana->value << " and " << bruno->value
                  << '\n';

        if (auto updated = page.update(*ana, bytes("Ana | Engineering | 15000 | adjusted"));
            !updated) {
            std::cerr << updated.error().message << '\n';
            return 1;
        }
        if (auto erased = page.erase(*bruno); !erased) {
            std::cerr << erased.error().message << '\n';
            return 1;
        }
        std::cout << "Updated slot " << ana->value << " and erased slot " << bruno->value << '\n';
        std::cout << "Free space after edits: " << page.free_space() << '\n';

        if (auto written = created->write(*page_id, page.page()); !written) {
            std::cerr << written.error().message << '\n';
            return 1;
        }
        if (auto flushed = created->flush(); !flushed) {
            std::cerr << flushed.error().message << '\n';
            return 1;
        }
    }

    auto checked = modb::storage::check_database(path);
    if (!checked) {
        std::cerr << checked.error().message << '\n';
        return 1;
    }
    std::cout << "Database check for the lab file: " << (checked->ok() ? "OK" : "FAILED")
              << '\n';
    return checked->ok() ? 0 : 1;
}
