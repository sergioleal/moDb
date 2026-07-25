#pragma once

// Importa Result e ErrorCode usados nas verificações de erro.
#include "modb/error.hpp"

// Disponibiliza a saída dos resultados no console.
#include <iostream>
// Disponibiliza o mutex que torna check() usável de várias threads.
#include <mutex>
// Disponibiliza mensagens leves sem cópia.
#include <string_view>

// Fornece as verificações mínimas compartilhadas pelos testes.
class TestSuite {
public:
    // Registra uma falha quando a condição recebida é falsa.
    void check(bool condition, std::string_view message) {
        // Não faz nada quando a condição é verdadeira.
        if (!condition) {
            // Os testes de rede passam a mesma TestSuite para a thread que roda
            // `serve_one()` e para a main, então `++failures_` acontecia de duas
            // threads: sem o mutex a soma pode perder uma falha e o teste passa
            // escondendo o defeito, além de embaralhar as linhas no stderr.
            const std::scoped_lock lock{mutex_};
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
        const std::scoped_lock lock{mutex_};
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
    // Serializa check()/finish() porque os testes de rede compartilham a suíte
    // entre a thread do servidor e a main. `mutable` para `finish()` continuar
    // const, como os testes já esperam.
    mutable std::mutex mutex_{};
};
