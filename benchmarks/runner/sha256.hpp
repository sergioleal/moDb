#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace modb::bench {

// Digest SHA-256 de 32 bytes.
using Sha256Digest = std::array<std::uint8_t, 32>;

// Calcula o SHA-256 de um buffer arbitrário.
[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data) noexcept;

// Calcula o SHA-256 de texto UTF-8.
[[nodiscard]] Sha256Digest sha256_text(std::string_view text) noexcept;

// Converte o digest para hex minúsculo (64 caracteres).
[[nodiscard]] std::string sha256_hex(const Sha256Digest& digest);

} // namespace modb::bench
