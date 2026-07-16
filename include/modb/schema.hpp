#pragma once

// Importa os tipos que uma coluna pode possuir.
#include "modb/data_type.hpp"
// Importa Result e os erros de validação.
#include "modb/error.hpp"

// Disponibiliza std::size_t.
#include <cstddef>
// Permite criar um schema usando uma lista entre chaves.
#include <initializer_list>
// Disponibiliza uma visão sem cópia das colunas.
#include <span>
// Disponibiliza o armazenamento dos nomes.
#include <string>
// Disponibiliza uma visão leve usada nas buscas.
#include <string_view>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza o contêiner que possui as colunas.
#include <vector>

namespace modb {

// Declara Row sem incluir seu header e evita uma dependência circular.
class Row;

// Descreve uma única coluna de uma tabela.
struct ColumnDefinition {
    // Guarda o nome usado para localizar a coluna.
    std::string name;
    // Define quais valores a coluna aceita.
    DataType type;
    // Informa se a coluna aceita NULL.
    bool nullable{true};

    // Permite comparar definições em testes.
    friend bool operator==(const ColumnDefinition&, const ColumnDefinition&) = default;
};

// Reúne e valida as definições das colunas de uma tabela.
class Schema {
public:
    // Cria um schema validado a partir de um vetor de colunas.
    [[nodiscard]] static Result<Schema> create(std::vector<ColumnDefinition> columns);
    // Facilita a criação usando uma lista entre chaves.
    [[nodiscard]] static Result<Schema> create(
        std::initializer_list<ColumnDefinition> columns);

    // Retorna a quantidade de colunas.
    [[nodiscard]] std::size_t column_count() const noexcept;
    // Expõe as colunas somente para leitura e sem cópia.
    [[nodiscard]] std::span<const ColumnDefinition> columns() const noexcept;
    // Procura uma coluna pelo nome e retorna seu índice.
    [[nodiscard]] Result<std::size_t> find_column(std::string_view name) const;
    // Confere se uma linha respeita quantidade, tipos e nulabilidade.
    [[nodiscard]] Result<void> validate(const Row& row) const;

private:
    // Somente create pode construir um schema, garantindo sua validação.
    explicit Schema(std::vector<ColumnDefinition> columns) : columns_{std::move(columns)} {}

    // Possui as definições na ordem das colunas.
    std::vector<ColumnDefinition> columns_;
};

} // namespace modb
