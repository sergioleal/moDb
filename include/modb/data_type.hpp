#pragma once

// Disponibiliza uma string leve que não possui o texto.
#include <string_view>

namespace modb {

// Lista os tipos de coluna inicialmente suportados pelo moDb.
enum class DataType {
    // Representa verdadeiro ou falso.
    boolean,
    // Representa um inteiro de 64 bits com sinal.
    integer,
    // Representa um número de ponto flutuante.
    real,
    // Representa um texto UTF-8.
    text,
};

// Retorna o nome SQL de um tipo, como "INTEGER".
[[nodiscard]] std::string_view data_type_name(DataType type) noexcept;

} // namespace modb
