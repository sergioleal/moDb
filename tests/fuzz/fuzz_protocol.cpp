// Alvo: decoder do protocolo de rede (frames hostis).

#include "fuzz_common.hpp"
#include "modb/net/protocol.hpp"

#include <cstdint>

namespace {

int fuzz_protocol(const std::uint8_t* data, std::size_t size) {
    const auto bytes = modb::fuzz::as_bytes(data, size);
    (void)modb::net::decode_message(bytes);
    return 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return fuzz_protocol(data, size);
}

MODB_FUZZ_DEFINE_MAIN(fuzz_protocol)
