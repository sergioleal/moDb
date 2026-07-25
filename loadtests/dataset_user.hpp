#pragma once

// Dataset `user_v1` (docs/PLANO_TESTES_DE_CARGA.md §7). Gerador determinístico:
// `at(seed, index)` é função pura de (seed, index) -- nunca depende de ordem
// de chamada nem de estado de PRNG entre chamadas. Isso é o que permite que
// create_delete_reverse/interleaved (subfases futuras) recriem exatamente o
// mesmo valor lógico para um índice, independentemente da ordem de mutação.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace modb::loadtest {

inline constexpr std::uint32_t dataset_user_version = 1;

struct GeneratedUser {
    std::int64_t id{};
    std::string login;
    std::string email;
    std::string display_name;
    std::int64_t created_at{};
    std::int32_t status{};
    std::vector<std::byte> filler;
};

// Tamanho do campo `filler` por payload (§4.5): slim ~64B, normal ~256B,
// fat ~4KiB. Um payload desconhecido cai em "normal".
[[nodiscard]] std::size_t filler_bytes_for_payload(std::string_view payload);

// Gera o usuário de índice `index` (1-based, igual ao `id`) determinado só
// por `seed` e `index` -- chamar `at(seed, 5)` duas vezes, em qualquer ordem
// e com qualquer histórico prévio, produz sempre o mesmo valor.
[[nodiscard]] GeneratedUser generate_user(std::uint64_t seed, std::uint64_t index,
                                          std::string_view payload);

// Linha canônica usada para o hash lógico do dataset (§9): concatenação
// estável dos campos, em texto, sem depender de como o valor foi
// codificado/decodificado pelo motor.
[[nodiscard]] std::string canonical_line(const GeneratedUser& user);

} // namespace modb::loadtest
