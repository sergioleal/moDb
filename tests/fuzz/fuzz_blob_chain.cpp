// Alvo: parser de página BLBP (magic/versão/length antes de seguir cadeia).

#include "fuzz_common.hpp"
#include "modb/object/blob_store.hpp"
#include "modb/storage/page.hpp"

#include <algorithm>
#include <cstdint>

namespace {

int fuzz_blob_chain(const std::uint8_t* data, std::size_t size) {
    const auto bytes = modb::fuzz::as_bytes(data, size);
    modb::storage::Page page{};
    const auto copy_n = std::min(bytes.size(), page.bytes().size());
    std::copy_n(bytes.begin(), copy_n, page.bytes().begin());
    (void)modb::object::parse_blob_page(page);
    return 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return fuzz_blob_chain(data, size);
}

MODB_FUZZ_DEFINE_MAIN(fuzz_blob_chain)
