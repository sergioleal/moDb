#pragma once

// Importa o tipo Value armazenado em cada posição da linha.
#include "modb/value.hpp"

// Disponibiliza std::size_t para tamanhos e índices.
#include <cstddef>
// Permite construir uma linha com uma lista entre chaves.
#include <initializer_list>
// Disponibiliza uma visão segura e sem cópia dos valores.
#include <span>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza o contêiner que possui os valores.
#include <vector>

namespace modb {

// Representa uma linha lógica como uma sequência ordenada de valores.
class Row {
public:
    // Cria uma linha vazia.
    Row() = default;
    // Permite escrever Row{Value{1}, Value{"Ana"}}.
    Row(std::initializer_list<Value> values) : values_{values} {}
    // Recebe um vetor existente sem exigir uma nova cópia.
    explicit Row(std::vector<Value> values) : values_{std::move(values)} {}

    // Retorna a quantidade de valores da linha.
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    // Expõe todos os valores somente para leitura e sem cópia.
    [[nodiscard]] std::span<const Value> values() const noexcept { return values_; }
    // Retorna um valor pelo índice e verifica os limites.
    [[nodiscard]] const Value& operator[](std::size_t index) const { return values_.at(index); }

    // Compara todos os valores de duas linhas.
    friend bool operator==(const Row&, const Row&) = default;

private:
    // Possui os valores armazenados na ordem das colunas.
    std::vector<Value> values_;
};

} // namespace modb
