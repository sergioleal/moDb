// Helpers não-template compartilhados por PersistentVector/Set/Map (Fase 4).
// O grosso das coleções é template e vive no header; aqui ficam os utilitários
// que não dependem do tipo do elemento.
#include "modb/object/collection.hpp"

namespace modb::object {

Result<std::uint32_t> collection_count(std::span<const std::byte> raw) {
    storage::BinaryReader reader{raw};
    return reader.read_u32();
}

bool encoded_less(std::span<const std::byte> a, std::span<const std::byte> b) {
    // Ordem lexicográfica por byte sem sinal — determinística e suficiente para
    // deduplicar (encoding igual ⇒ valor igual) e para a busca binária.
    return std::lexicographical_compare(
        a.begin(), a.end(), b.begin(), b.end(),
        [](std::byte left, std::byte right) {
            return std::to_integer<unsigned char>(left) < std::to_integer<unsigned char>(right);
        });
}

} // namespace modb::object
