#include "modb/storage/async_file.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
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

} // namespace

int main() {
    TestSuite suite;
    TemporaryFile temp{"roundtrip"};

    {
        AsyncFileOptions options;
        options.max_inflight = 4;
        auto opened = AsyncFile::open(temp.path(), AsyncFile::Mode::create_new, options);
        suite.check(opened.has_value(), "open create_new");
        if (!opened) {
            return suite.finish();
        }
        suite.check(!opened->backend_name().empty(), "backend_name non-empty");
        suite.check(opened->backend() == IoBackend::sync_fallback ||
                        opened->backend() == IoBackend::iocp ||
                        opened->backend() == IoBackend::io_uring,
                    "known backend");
        suite.check(!opened->fallback_reason().empty() || opened->backend() != IoBackend::sync_fallback,
                    "fallback reason when sync");

        const std::string payload = "wal-then-pages";
        std::vector<std::byte> bytes(payload.size());
        for (std::size_t i = 0; i < payload.size(); ++i) {
            bytes[i] = static_cast<std::byte>(payload[i]);
        }

        // Ordering: write → sync barrier → (simula páginas) write → barrier → read
        suite.check(opened->submit_write_at(0, bytes).has_value(), "submit wal write");
        suite.check(opened->submit_sync().has_value(), "submit wal sync");
        suite.check(opened->barrier().has_value(), "barrier after wal");

        const std::string page = "PAGE";
        std::vector<std::byte> page_bytes(page.size());
        for (std::size_t i = 0; i < page.size(); ++i) {
            page_bytes[i] = static_cast<std::byte>(page[i]);
        }
        suite.check(opened->submit_write_at(64, page_bytes).has_value(), "submit page write");
        suite.check(opened->barrier().has_value(), "barrier after pages");

        std::vector<std::byte> read_back(payload.size());
        suite.check(opened->read_at(0, read_back).has_value(), "read_at");
        std::string got;
        got.resize(read_back.size());
        for (std::size_t i = 0; i < read_back.size(); ++i) {
            got[i] = static_cast<char>(read_back[i]);
        }
        suite.check(got == payload, "roundtrip payload");

        suite.check(opened->cancel_all().has_value(), "cancel empty queue");
        suite.check(opened->close().has_value(), "close");
    }

    {
        auto reopened = AsyncFile::open(temp.path(), AsyncFile::Mode::open_existing);
        suite.check(reopened.has_value(), "reopen");
        if (reopened) {
            std::vector<std::byte> page(4);
            suite.check(reopened->read_at(64, page).has_value(), "read page after reopen");
            suite.check(static_cast<char>(page[0]) == 'P' && static_cast<char>(page[3]) == 'E',
                        "page bytes");
        }
    }

    {
        TemporaryFile limited{"limit"};
        AsyncFileOptions options;
        options.max_inflight = 2;
        auto file = AsyncFile::open(limited.path(), AsyncFile::Mode::create_new, options);
        suite.check(file.has_value(), "open limited");
        if (file) {
            std::vector<std::byte> one{std::byte{1}};
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
        AsyncFileOptions options;
        options.require_async = true;
        TemporaryFile req{"require"};
        auto file = AsyncFile::open(req.path(), AsyncFile::Mode::create_new, options);
        // Neste build o backend é sync_fallback → require_async deve falhar.
        if (file) {
            suite.check(file->backend() != IoBackend::sync_fallback,
                        "require_async succeeded only with real async");
        } else {
            suite.check(file.error().code == ErrorCode::io_error, "require_async → io_error");
        }
    }

    return suite.finish();
}
