// Importa o WAL exercitado aqui.
#include "modb/tx/wal.hpp"
// Importa Page/page_size para as imagens de página.
#include "modb/storage/page.hpp"
// Importa as verificações mínimas.
#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace modb;
using namespace modb::tx;

namespace {

class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-wal-" + std::to_string(unique) + "-" + std::string{suffix} + ".wal");
    }
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

storage::Page page_filled(std::byte value) {
    storage::Page page;
    for (auto& byte : page.bytes()) {
        byte = value;
    }
    return page;
}

void overwrite_byte(const std::filesystem::path& path, std::streamoff offset, char value) {
    std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
    stream.seekp(offset);
    stream.put(value);
}

} // namespace

int main() {
    TestSuite suite;

    // --- round-trip de begin/image/commit ---
    {
        TemporaryFile file{"roundtrip"};
        const auto page = page_filled(std::byte{0xAB});
        {
            auto wal = Wal::create(file.path());
            suite.check(wal.has_value(), "WAL is created");
            if (!wal) {
                return suite.finish();
            }
            suite.check(wal->append_begin(1).has_value(), "begin appended");
            suite.check(wal->append_page_image(1, storage::PageId{5}, page.bytes()).has_value(),
                        "page image appended");
            suite.check(wal->append_commit(1).has_value(), "commit appended");
            suite.check(wal->sync().has_value(), "WAL synced");
        }
        auto records = Wal::read_all(file.path());
        suite.check(records.has_value() && records->size() == 3,
                    "three records are read back");
        if (records && records->size() == 3) {
            const auto& begin = (*records)[0];
            const auto& image = (*records)[1];
            const auto& commit = (*records)[2];
            suite.check(begin.type == WalRecordType::begin && begin.tx_id == 1 && begin.lsn == 1,
                        "begin record round-trips");
            suite.check(image.type == WalRecordType::page_image && image.page_id == 5 &&
                            image.payload.size() == storage::page_size &&
                            image.payload.front() == std::byte{0xAB},
                        "page image round-trips with its payload");
            suite.check(commit.type == WalRecordType::commit && commit.lsn == 3,
                        "commit record round-trips with a monotonic LSN");
        }
    }

    // --- CRC corrompido marca o fim lógico ---
    {
        TemporaryFile file{"crc"};
        const auto page = page_filled(std::byte{0x11});
        {
            auto wal = Wal::create(file.path());
            if (!wal) {
                return suite.finish();
            }
            (void)wal->append_begin(7);
            (void)wal->append_page_image(7, storage::PageId{2}, page.bytes());
            (void)wal->append_commit(7);
            (void)wal->sync();
        }
        // Corrompe um byte dentro do payload do segundo registro (a imagem):
        // header 32 + primeiro registro (29) + área do payload da imagem.
        const auto image_payload_offset = static_cast<std::streamoff>(32 + 29 + 29 + 10);
        overwrite_byte(file.path(), image_payload_offset, static_cast<char>(0xFF));
        auto records = Wal::read_all(file.path());
        suite.check(records.has_value() && records->size() == 1,
                    "reading stops at the record before the corrupted one");
    }

    // --- WAL truncado no meio de um registro ---
    {
        TemporaryFile file{"truncated"};
        const auto page = page_filled(std::byte{0x22});
        {
            auto wal = Wal::create(file.path());
            if (!wal) {
                return suite.finish();
            }
            (void)wal->append_begin(9);
            (void)wal->append_page_image(9, storage::PageId{3}, page.bytes());
            (void)wal->append_commit(9);
            (void)wal->sync();
        }
        // Corta os últimos bytes: o registro de commit fica truncado.
        std::error_code resize_error;
        const auto size = std::filesystem::file_size(file.path(), resize_error);
        std::filesystem::resize_file(file.path(), size - 5, resize_error);
        suite.check(!resize_error, "WAL file is truncated for the test");
        auto records = Wal::read_all(file.path());
        suite.check(records.has_value() && records->size() == 2,
                    "reading stops at the last intact record");
    }

    return suite.finish();
}
