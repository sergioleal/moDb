// Importa a função sanitizadora exercitada neste teste.
#include "modb/text_escape.hpp"

// Importa as funções simples de verificação.
#include "test_support.hpp"

// Disponibiliza a montagem de entradas com bytes de controle.
#include <string>

// Executa os cenários de escape de texto para exibição no terminal.
int main() {
    // Evita repetir o namespace da função.
    using namespace modb;

    // Acumula e mostra as falhas encontradas.
    TestSuite suite;

    // Texto vazio continua vazio.
    suite.check(escape_for_terminal("") == "", "empty text stays empty");

    // Texto imprimível comum passa sem alteração.
    suite.check(escape_for_terminal("Ana Maria 123") == "Ana Maria 123",
                "printable ASCII is preserved");

    // O byte ESC, base de qualquer sequência ANSI/OSC, vira a forma visível.
    suite.check(escape_for_terminal(std::string{"antes\x1b[2Jdepois"}) == "antes\\x1b[2Jdepois",
                "ESC byte is escaped");

    // Uma sequência OSC que redefine o título do terminal fica inerte.
    suite.check(escape_for_terminal(std::string{"\x1b]0;dono\x07"}) == "\\x1b]0;dono\\x07",
                "OSC title-change sequence is neutralized");

    // Nova linha e tabulação são escapadas para não forjar linhas na saída.
    suite.check(escape_for_terminal(std::string{"linha1\nlinha2\tfim"}) ==
                    "linha1\\x0alinha2\\x09fim",
                "newline and tab are escaped to prevent output spoofing");

    // O retorno de carro e o DEL também são bytes de controle escapados.
    suite.check(escape_for_terminal(std::string{"a\rb\x7f"}) == "a\\x0db\\x7f",
                "carriage return and DEL are escaped");

    // A barra invertida é dobrada para a saída ficar sem ambiguidade.
    suite.check(escape_for_terminal("c:\\temp") == "c:\\\\temp",
                "backslash is doubled");

    // Um byte nulo no meio do texto não interrompe a sanitização.
    suite.check(escape_for_terminal(std::string{"a\0b", 3}) == "a\\x00b",
                "embedded NUL byte is escaped without truncating");

    // Bytes UTF-8 (>= 0x80) são preservados para não corromper acentuação.
    suite.check(escape_for_terminal("Jos\xc3\xa9") == "Jos\xc3\xa9",
                "UTF-8 multibyte sequences are preserved");

    // Encerra o processo com o resultado acumulado.
    return suite.finish();
}
