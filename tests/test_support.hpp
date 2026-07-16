#pragma once

// Importa Result e ErrorCode usados nas verificações de erro.
#include "modb/error.hpp"

// Disponibiliza a saída dos resultados no console.
#include <iostream>
// Disponibiliza mensagens leves sem cópia.
#include <string_view>

// Fornece as verificações mínimas compartilhadas pelos testes.
class TestSuite {
public:
    // Registra uma falha quando a condição recebida é falsa.
    void check(bool condition, std::string_view message) {
        // Não faz nada quando a condição é verdadeira.
        if (!condition) {
            // Soma uma falha ao resultado final.
            ++failures_;
            // Mostra qual expectativa não foi atendida.
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    // Verifica se um Result falhou com o código esperado.
    template <typename T>
    void check_error(const modb::Result<T>& result, modb::ErrorCode expected,
                     std::string_view message) {
        // O operador && evita acessar error quando existe um valor de sucesso.
        check(!result && result.error().code == expected, message);
    }

    // Converte a quantidade de falhas em um código de saída do processo.
    [[nodiscard]] int finish() const {
        // Mostra uma confirmação quando nenhuma verificação falhou.
        if (failures_ == 0) {
            std::cout << "All tests passed\n";
        }
        // Retorna zero no sucesso e um na falha.
        return failures_ == 0 ? 0 : 1;
    }

private:
    // Conta quantas verificações falharam.
    int failures_{0};
};
