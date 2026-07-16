#pragma once

// Disponibiliza std::byte, std::to_integer e std::size_t.
#include <cstddef>
// Disponibiliza os inteiros de largura fixa manipulados aqui.
#include <cstdint>
// Restringe as funções a inteiros sem sinal (little-endian bem definido).
#include <concepts>
// Disponibiliza a visão contígua de bytes usada como destino e origem.
#include <span>

namespace modb::storage {

// Grava value em little-endian nos primeiros sizeof(T) bytes de destination.
// Precondição do chamador: destination possui ao menos sizeof(T) bytes.
template <std::unsigned_integral T>
constexpr void store_le(std::span<std::byte> destination, T value) noexcept {
    // Escreve um byte por vez, do menos significativo para o mais significativo.
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        // Mantém apenas o byte menos significativo atual.
        destination[index] = std::byte{static_cast<std::uint8_t>(value & static_cast<T>(0xffU))};
        // Descarta o byte já gravado, preservando o tipo sem sinal.
        value = static_cast<T>(value >> 8U);
    }
}

// Reconstrói um T little-endian a partir dos primeiros sizeof(T) bytes de source.
// Precondição do chamador: source possui ao menos sizeof(T) bytes.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T load_le(std::span<const std::byte> source) noexcept {
    // Começa com todos os bits desligados.
    T value = 0;
    // Recoloca cada byte em sua posição original.
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        // Converte o byte e o desloca de volta para sua casa no inteiro.
        const auto shifted = static_cast<T>(
            static_cast<T>(std::to_integer<std::uint8_t>(source[index])) << (index * 8U));
        // Combina com os bytes já reconstruídos.
        value = static_cast<T>(value | shifted);
    }
    // Devolve o inteiro reconstruído.
    return value;
}

} // namespace modb::storage
