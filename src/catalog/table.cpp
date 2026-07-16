// Importa a declaração de Table.
#include "modb/table.hpp"

// Disponibiliza std::move para transferir linhas.
#include <utility>

namespace modb {

// Valida e armazena uma nova linha.
Result<void> Table::insert(Row row) {
    // Pede ao schema que confira quantidade, tipos e nulabilidade.
    if (auto result = schema_.validate(row); !result) {
        // Propaga o mesmo erro quando a linha é inválida.
        return result;
    }
    // Move a linha válida para o vetor da tabela.
    rows_.push_back(std::move(row));
    // Informa que a inserção terminou com sucesso.
    return {};
}

// Expõe as linhas somente para leitura e sem copiar o vetor.
std::span<const Row> Table::scan() const noexcept { return rows_; }

// Retorna uma visão do nome sem copiar a string.
std::string_view Table::name() const noexcept { return name_; }

// Retorna uma referência constante para o schema da tabela.
const Schema& Table::schema() const noexcept { return schema_; }

} // namespace modb
