#pragma once

// Política de compatibilidade (Fase 10E / ADR-015): major incompatível;
// minor aditivo — leitor com major igual e minor >= do arquivo/peer lê.

#include "modb/error.hpp"

#include <cstdint>
#include <string>

namespace modb {

struct CompatibilityVersion {
    std::uint16_t major{0};
    std::uint16_t minor{0};

    friend bool operator==(const CompatibilityVersion&, const CompatibilityVersion&) = default;
};

// Codifica major/minor num u16 de fio/disco compatível com o legado:
//   minor == 0 → grava só o major (ex.: 1)
//   minor  > 0 → (major << 8) | minor
[[nodiscard]] constexpr std::uint16_t to_wire_u16(CompatibilityVersion version) noexcept {
    if (version.minor == 0) {
        return version.major;
    }
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(version.major << 8) | version.minor));
}

// Decodifica o u16 legado ou empacotado.
[[nodiscard]] constexpr CompatibilityVersion from_wire_u16(std::uint16_t wire) noexcept {
    if (wire < 256) {
        return CompatibilityVersion{wire, 0};
    }
    return CompatibilityVersion{static_cast<std::uint16_t>(wire >> 8),
                                static_cast<std::uint16_t>(wire & 0xffU)};
}

// Arquivo/peer legível se major igual e minor do artefato ≤ minor do leitor.
[[nodiscard]] inline Result<void> ensure_readable(CompatibilityVersion artifact,
                                                  CompatibilityVersion reader, ErrorCode code,
                                                  const char* label) {
    if (artifact.major != reader.major) {
        return std::unexpected(Error{
            code,
            std::string{label} + " major " + std::to_string(artifact.major) +
                " incompatible with reader major " + std::to_string(reader.major),
        });
    }
    if (artifact.minor > reader.minor) {
        return std::unexpected(Error{
            code,
            std::string{label} + " minor " + std::to_string(artifact.minor) +
                " requires reader minor >= " + std::to_string(artifact.minor) + " (have " +
                std::to_string(reader.minor) + ")",
        });
    }
    return {};
}

// Negocia a versão efetiva do protocolo: major deve coincidir; minor = min(ambas).
[[nodiscard]] inline Result<CompatibilityVersion> negotiate_protocol_version(
    CompatibilityVersion client, CompatibilityVersion server) {
    if (client.major != server.major) {
        return std::unexpected(Error{
            ErrorCode::incompatible_protocol_version,
            "protocol major " + std::to_string(client.major) + " incompatible with server major " +
                std::to_string(server.major),
        });
    }
    const auto minor =
        client.minor < server.minor ? client.minor : server.minor;
    return CompatibilityVersion{server.major, minor};
}

} // namespace modb
