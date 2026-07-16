#pragma once

// Disponibiliza a entrada como visão leve e a saída como texto próprio.
#include <string>
#include <string_view>

namespace modb {

// Escapa bytes perigosos de um TEXT antes de exibi-lo em um terminal.
//
// Um arquivo .db é entrada não confiável e o codec aceita qualquer sequência de
// bytes em um valor TEXT. Imprimir esses bytes crus permitiria que um banco
// malicioso injetasse sequências de escape ANSI/OSC (mover cursor, apagar tela,
// redefinir o título) ou quebras de linha que forjam linhas na saída da CLI.
//
// A função devolve uma cópia em que todo byte de controle C0 (0x00–0x1f),
// incluindo tabulação e nova linha, e o DEL (0x7f) viram a forma visível \xHH; a
// barra invertida é dobrada para manter a saída sem ambiguidade. Bytes acima de
// 0x7f são preservados para não corromper texto UTF-8 legítimo.
[[nodiscard]] std::string escape_for_terminal(std::string_view text);

} // namespace modb
