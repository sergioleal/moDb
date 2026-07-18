#pragma once

// Codificação de chave ORDENÁVEL BYTE A BYTE para os índices (Fase 7B): dado um
// AttributeValue, produz bytes tais que a ordem lexicográfica (memcmp) das
// codificações reproduz a ordem lógica dos valores. Assim a B+ tree compara
// chaves sem conhecer o tipo — só compara bytes. Um índice cobre um único tipo
// de atributo, então todas as chaves de uma árvore compartilham o mesmo
// encoding.

// Importa Result e códigos de erro.
#include "modb/error.hpp"
// Importa AttributeValue e AttributeType.
#include "modb/object/attribute_value.hpp"

// Disponibiliza std::bit_cast para o encoding de float.
#include <bit>
// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza o vetor de bytes devolvido.
#include <vector>

namespace modb::index {

// Grava um u64 em big-endian (mais significativo primeiro) — a ordem de bytes
// que preserva a ordem numérica sob memcmp.
inline void store_be64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(std::byte{static_cast<std::uint8_t>((value >> shift) & 0xffU)});
    }
}

// Codifica um valor indexável em bytes ordenáveis:
// - int64: big-endian de (bits XOR bit de sinal) — negativos antes de positivos;
// - float64: IEEE-754 com flip do sinal (ou de todos os bits, se negativo),
//   dando ordem total correta entre negativos e positivos;
// - string/bytes: bytes crus (a ordem lexicográfica já é a desejada);
// - bool: 0 ou 1; ref/blob: big-endian do u64 (ids são sem sinal).
// null e embedded não são indexáveis.
[[nodiscard]] inline Result<std::vector<std::byte>> encode_key(
    const modb::object::AttributeValue& value) {
    using modb::object::AttributeType;
    std::vector<std::byte> out;
    switch (value.type()) {
    case AttributeType::boolean: {
        auto raw = value.as_bool();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        out.push_back(std::byte{static_cast<std::uint8_t>(*raw ? 1 : 0)});
        return out;
    }
    case AttributeType::int64: {
        auto raw = value.as_int64();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        // Flip do bit de sinal: INT64_MIN vira 0x00.., INT64_MAX vira 0xFF..
        const auto biased = static_cast<std::uint64_t>(*raw) ^ (std::uint64_t{1} << 63);
        store_be64(out, biased);
        return out;
    }
    case AttributeType::float64: {
        auto raw = value.as_float64();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        auto bits = std::bit_cast<std::uint64_t>(*raw);
        // Negativo (bit de sinal ligado): inverte tudo, para que -1 < -0.5 etc.
        // Positivo: liga o bit de sinal, para vir depois de qualquer negativo.
        if ((bits & (std::uint64_t{1} << 63)) != 0) {
            bits = ~bits;
        } else {
            bits |= (std::uint64_t{1} << 63);
        }
        store_be64(out, bits);
        return out;
    }
    case AttributeType::string: {
        auto raw = value.as_string();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        out.reserve(raw->size());
        for (char c : *raw) {
            out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        }
        return out;
    }
    case AttributeType::ref: {
        auto raw = value.as_ref();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        store_be64(out, raw->value);
        return out;
    }
    case AttributeType::blob: {
        auto raw = value.as_blob();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        store_be64(out, raw->value);
        return out;
    }
    case AttributeType::null:
    case AttributeType::bytes:
    case AttributeType::embedded:
        break;
    }
    return std::unexpected(Error{ErrorCode::invalid_argument,
                                 "attribute type is not indexable"});
}

} // namespace modb::index
