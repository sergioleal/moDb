// Alvo: Wal::read_all sobre arquivo temporário com bytes hostis.

#include "fuzz_common.hpp"
#include "modb/tx/wal.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

int fuzz_wal(const std::uint8_t* data, std::size_t size) {
    const auto bytes = modb::fuzz::as_bytes(data, size);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("modb-fuzz-wal-" + std::to_string(stamp) + ".wal");
    {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        if (!out) {
            return 0;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    (void)modb::tx::Wal::read_all(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return fuzz_wal(data, size);
}

MODB_FUZZ_DEFINE_MAIN(fuzz_wal)
