// Alvo: sub-formato de atributos do catálogo + TypeDefinition::create.

#include "fuzz_common.hpp"
#include "modb/object/catalog_store.hpp"
#include "modb/object/type_definition.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace {

int fuzz_type_definition(const std::uint8_t* data, std::size_t size) {
    const auto bytes = modb::fuzz::as_bytes(data, size);
    auto attributes = modb::object::decode_type_attributes(bytes);
    if (!attributes) {
        return 0;
    }
    // Nome curto derivado dos primeiros bytes (limites já validados no decoder).
    std::string name = "T";
    if (!bytes.empty()) {
        const auto ch = static_cast<unsigned char>(bytes[0]);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            name.push_back(static_cast<char>(ch));
        }
    }
    (void)modb::object::TypeDefinition::create(std::move(name), std::move(*attributes));
    return 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return fuzz_type_definition(data, size);
}

MODB_FUZZ_DEFINE_MAIN(fuzz_type_definition)
