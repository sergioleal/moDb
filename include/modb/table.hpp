#pragma once

// Importa Result para informar falhas de inserção.
#include "modb/error.hpp"
// Importa as linhas armazenadas pela tabela.
#include "modb/row.hpp"
// Importa a descrição e a validação das colunas.
#include "modb/schema.hpp"

// Disponibiliza uma visão sem cópia das linhas.
#include <span>
// Disponibiliza o armazenamento do nome.
#include <string>
// Disponibiliza uma visão leve do nome.
#include <string_view>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza o contêiner que possui as linhas.
#include <vector>

namespace modb {

// Representa uma tabela relacional mantida em memória.
class Table {
public:
    // Recebe o nome e um schema que já foi validado.
    Table(std::string name, Schema schema) : name_{std::move(name)}, schema_{std::move(schema)} {}

    // Valida e adiciona uma linha à tabela.
    [[nodiscard]] Result<void> insert(Row row);
    // Retorna uma visão somente leitura de todas as linhas.
    [[nodiscard]] std::span<const Row> scan() const noexcept;
    // Retorna o nome da tabela sem copiá-lo.
    [[nodiscard]] std::string_view name() const noexcept;
    // Retorna o schema somente para leitura.
    [[nodiscard]] const Schema& schema() const noexcept;

private:
    // Guarda o nome da tabela.
    std::string name_;
    // Guarda a definição das colunas.
    Schema schema_;
    // Guarda temporariamente as linhas em memória.
    std::vector<Row> rows_;
};

} // namespace modb
