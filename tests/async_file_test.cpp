#include "modb/storage/async_file.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace modb;
using namespace modb::storage;

namespace {

class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-async-" + std::to_string(unique) + "-" + std::string{suffix} + ".bin");
    }
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::vector<std::byte> bytes_from(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<std::byte>(text[i]);
    }
    return bytes;
}

std::string string_from(std::span<const std::byte> bytes) {
    std::string text;
    text.resize(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        text[i] = static_cast<char>(bytes[i]);
    }
    return text;
}

} // namespace

int main() {
    TestSuite suite;

    {
        TemporaryFile temp{"roundtrip"};
        AsyncFileOptions options;
        options.max_inflight = 8;
        options.require_async = true;
        auto opened = AsyncFile::open(temp.path(), AsyncFile::Mode::create_new, options);
        suite.check(opened.has_value(), "open native async backend");
        if (!opened) {
            return suite.finish();
        }

#ifdef _WIN32
        suite.check(opened->backend() == IoBackend::iocp, "windows uses IOCP");
#else
        suite.check(opened->backend() == IoBackend::posix_aio, "linux uses POSIX AIO");
#endif
        suite.check(opened->fallback_reason().empty(), "no fallback on native backend");

        const auto wal = bytes_from("wal-then-pages");
        const auto page = bytes_from("PAGE");
        suite.check(opened->submit_write_at(0, wal).has_value(), "submit wal write");
        suite.check(opened->submit_sync().has_value(), "submit wal sync");
        suite.check(opened->submit_write_at(64, page).has_value(), "submit page write");
        suite.check(opened->barrier().has_value(), "barrier applies ordered groups");

        std::vector<std::byte> wal_read(wal.size());
        std::vector<std::byte> page_read(page.size());
        suite.check(opened->submit_read_at(0, wal_read).has_value(), "submit wal read");
        suite.check(opened->submit_read_at(64, page_read).has_value(), "submit page read");
        suite.check(opened->drain().has_value(), "drain reads");
        suite.check(string_from(wal_read) == "wal-then-pages", "wal roundtrip");
        suite.check(string_from(page_read) == "PAGE", "page roundtrip");
        suite.check(opened->close().has_value(), "close");

        auto reopened = AsyncFile::open(temp.path(), AsyncFile::Mode::open_existing, options);
        suite.check(reopened.has_value(), "reopen async file");
        if (reopened) {
            std::vector<std::byte> read_back(page.size());
            suite.check(reopened->read_at(64, read_back).has_value(), "read after reopen");
            suite.check(string_from(read_back) == "PAGE", "durable page bytes");
        }
    }

    {
        TemporaryFile temp{"limit"};
        AsyncFileOptions options;
        options.max_inflight = 2;
        auto file = AsyncFile::open(temp.path(), AsyncFile::Mode::create_new, options);
        suite.check(file.has_value(), "open limited");
        if (file) {
            const auto one = bytes_from("x");
            suite.check(file->submit_write_at(0, one).has_value(), "inflight 1");
            suite.check(file->submit_write_at(1, one).has_value(), "inflight 2");
            auto overflow = file->submit_write_at(2, one);
            suite.check(!overflow && overflow.error().code == ErrorCode::invalid_argument,
                        "max_inflight exceeded");
            suite.check(file->cancel_all().has_value(), "cancel pending");
            suite.check(file->inflight() == 0, "queue cleared");
            suite.check(file->write_at(0, one).has_value(), "write after cancel");
        }
    }

    {
        TemporaryFile temp{"missing"};
        auto missing = AsyncFile::open(temp.path(), AsyncFile::Mode::open_existing);
        suite.check(!missing && missing.error().code == ErrorCode::io_error,
                    "open missing returns io_error");
    }

    return suite.finish();
}
