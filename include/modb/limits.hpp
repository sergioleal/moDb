#pragma once

// Disponibiliza std::size_t para representar tamanhos em bytes.
#include <cstddef>

namespace modb {

// Define o tamanho máximo compartilhado pelos nomes de tabelas e colunas.
inline constexpr std::size_t max_identifier_bytes = 63;

// Limita a quantidade de colunas para manter schemas e linhas controlados.
inline constexpr std::size_t max_columns_per_table = 256;

} // namespace modb
