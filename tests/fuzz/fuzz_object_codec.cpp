// Alvo: codec de objeto (header, payload e registro completo).

#include "fuzz_common.hpp"
#include "modb/object/object_codec.hpp"

#include <cstdint>

namespace {

int fuzz_object_codec(const std::uint8_t* data, std::size_t size) {
    const auto bytes = modb::fuzz::as_bytes(data, size);
    (void)modb::object::decode_object_header(bytes);
    (void)modb::object::decode_object_payload(bytes);
    (void)modb::object::decode_object(bytes);
    return 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return fuzz_object_codec(data, size);
}

MODB_FUZZ_DEFINE_MAIN(fuzz_object_codec)
