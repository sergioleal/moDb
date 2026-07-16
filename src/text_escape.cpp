// Importa a declaração da função de sanitização.
#include "modb/text_escape.hpp"

// Disponibiliza inteiros sem sinal de largura conhecida para o byte lido.
#include <cstdint>

namespace modb {
namespace {

// Tabela usada para converter um nibble em seu dígito hexadecimal minúsculo.
constexpr char hex_digits[] = "0123456789abcdef";

} // namespace

// Escapa bytes de controle e a barra invertida, preservando o restante.
std::string escape_for_terminal(std::string_view text) {
    // Reserva o tamanho original; só cresce quando há bytes a escapar.
    std::string result;
    result.reserve(text.size());
    // Examina cada byte sem interpretar sua codificação.
    for (const char character : text) {
        // Trata o byte como valor sem sinal para comparar as faixas de controle.
        const auto byte = static_cast<std::uint8_t>(character);
        // A barra invertida é dobrada para a saída permanecer sem ambiguidade.
        if (byte == static_cast<std::uint8_t>('\\')) {
            result += "\\\\";
        } else if (byte < 0x20U || byte == 0x7fU) {
            // Bytes de controle C0 e DEL viram a forma visível \xHH.
            result += "\\x";
            result += hex_digits[byte >> 4U];
            result += hex_digits[byte & 0x0fU];
        } else {
            // Texto imprimível e bytes UTF-8 (>= 0x80) passam sem alteração.
            result += character;
        }
    }
    // Devolve a cópia segura para impressão.
    return result;
}

} // namespace modb
