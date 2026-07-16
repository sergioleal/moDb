// Importa DataType e a declaração de data_type_name.
#include "modb/data_type.hpp"

namespace modb {

// Converte o valor interno do enum para o nome usado no SQL.
std::string_view data_type_name(DataType type) noexcept {
    // Escolhe o texto correspondente ao tipo recebido.
    switch (type) {
    case DataType::boolean:
        // BOOLEAN representa verdadeiro ou falso.
        return "BOOLEAN";
    case DataType::integer:
        // INTEGER representa inteiros de 64 bits.
        return "INTEGER";
    case DataType::real:
        // REAL representa números de ponto flutuante.
        return "REAL";
    case DataType::text:
        // TEXT representa textos.
        return "TEXT";
    }
    // Protege a função caso um valor inválido seja convertido para o enum.
    return "UNKNOWN";
}

} // namespace modb
