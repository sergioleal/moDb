// Importa as funções de nome e versão testadas aqui.
#include "modb/version.hpp"

// Disponibiliza a saída dos resultados no console.
#include <iostream>
// Disponibiliza mensagens leves para as verificações.
#include <string_view>

namespace {

// Retorna zero para uma condição verdadeira e um para uma falha.
int check(bool condition, std::string_view message) {
    // Uma condição verdadeira não precisa imprimir erro.
    if (condition) {
        return 0;
    }

    // Mostra a descrição da condição que falhou.
    std::cerr << "FAIL: " << message << '\n';
    // Um representa uma falha encontrada.
    return 1;
}

} // namespace

// Executa todas as verificações deste arquivo.
int main() {
    // Começa sem falhas.
    int failures = 0;
    // Confere se o nome público continua estável.
    failures += check(modb::project_name() == "moDb", "project name must be moDb");
    // Confere se o projeto sempre informa alguma versão.
    failures += check(!modb::project_version().empty(), "project version must not be empty");

    // Mostra a confirmação apenas quando todas as verificações passaram.
    if (failures == 0) {
        std::cout << "All tests passed\n";
    }
    // Converte a quantidade de falhas no código de saída do teste.
    return failures == 0 ? 0 : 1;
}
