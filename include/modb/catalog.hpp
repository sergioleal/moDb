#pragma once

// Importa Result e os códigos de erro.
#include "modb/error.hpp"
// Importa o tipo de linha aceito nas inserções.
#include "modb/row.hpp"
// Importa a definição do schema usado para criar tabelas.
#include "modb/schema.hpp"
// Importa a classe Table gerenciada pelo catálogo.
#include "modb/table.hpp"

// Disponibiliza std::reference_wrapper para retornar tabelas sem copiá-las.
#include <functional>
// Disponibiliza uma visão sem cópia das linhas.
#include <span>
// Disponibiliza o armazenamento dos nomes.
#include <string>
// Disponibiliza uma visão leve usada nas buscas.
#include <string_view>
// Disponibiliza o mapa que relaciona nomes e tabelas.
#include <unordered_map>

namespace modb {

// Mantém o conjunto de tabelas conhecidas pelo banco em memória.
class Catalog {
public:
    // Cria uma tabela após validar seu nome e sua unicidade.
    [[nodiscard]] Result<void> create_table(std::string name, Schema schema);
    // Insere uma linha na tabela indicada.
    [[nodiscard]] Result<void> insert(std::string_view table_name, Row row);
    // Procura uma tabela e retorna acesso somente leitura.
    [[nodiscard]] Result<std::reference_wrapper<const Table>> table(std::string_view name) const;
    // Retorna todas as linhas da tabela indicada.
    [[nodiscard]] Result<std::span<const Row>> scan(std::string_view table_name) const;

private:
    // Procura uma tabela permitindo que seus dados sejam alterados internamente.
    [[nodiscard]] Result<std::reference_wrapper<Table>> mutable_table(std::string_view name);

    // Relaciona o nome de cada tabela ao seu objeto em memória.
    std::unordered_map<std::string, Table> tables_;
};

} // namespace modb
