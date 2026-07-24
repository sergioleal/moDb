#include "modb/storage/async_file.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::filesystem::path temp_path() {
    // Async I/O writes to a temporary binary file, not a full database file.
    return std::filesystem::temp_directory_path() /
           ("ring0-phase-13-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
}

std::vector<std::byte> bytes(std::string_view text) {
    // The AsyncFile API works with raw bytes at explicit offsets.
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(text[i]);
    }
    return out;
}

} // namespace

int main() {
    std::cout << "Objective: write data with positional asynchronous I/O and an explicit barrier.\n";

    const auto path = temp_path();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    // require_async asks the platform backend to use its native async path.
    modb::storage::AsyncFileOptions options;
    options.max_inflight = 4;
    options.require_async = true;
    auto file = modb::storage::AsyncFile::open(path, modb::storage::AsyncFile::Mode::create_new,
                                               options);
    if (!file) {
        std::cerr << file.error().message << '\n';
        return 1;
    }

    const auto wal = bytes("wal");
    const auto page = bytes("page");
    // Submit positional writes plus an explicit sync without waiting after each call.
    if (!file->submit_write_at(0, wal) || !file->submit_sync() ||
        !file->submit_write_at(64, page)) {
        std::cerr << "failed to submit async operations\n";
        std::filesystem::remove(path, ignored);
        return 1;
    }
    // barrier() waits until all submitted operations are complete.
    auto drained = file->barrier();
    if (!drained) {
        std::cerr << drained.error().message << '\n';
        return 1;
    }
    std::cout << "backend=" << file->backend_name() << " max_inflight=" << file->max_inflight()
              << '\n';
    if (!file->close()) {
        std::cerr << "failed to close async file\n";
        return 1;
    }
    std::filesystem::remove(path, ignored);
    return 0;
}
