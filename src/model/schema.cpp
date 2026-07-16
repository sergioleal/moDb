// Importa Schema e ColumnDefinition.
#include "modb/schema.hpp"

// Importa o limite compartilhado para nomes de tabelas e colunas.
#include "modb/limits.hpp"
// Importa Row para validar seus valores.
#include "modb/row.hpp"

// Disponibiliza a montagem de mensagens com números.
#include <sstream>
// Disponibiliza um conjunto usado para detectar nomes repetidos.
#include <unordered_set>

namespace modb {
namespace {

// Valida apenas as regras do nome de uma coluna.
Result<void> validate_column_name(std::string_view name) {
    // Rejeita um nome vazio ou maior que o limite atual.
    if (name.empty() || name.size() > max_identifier_bytes) {
        // std::unexpected representa a parte de erro de Result.
        return std::unexpected(Error{
            ErrorCode::invalid_identifier,
            "column name must contain between 1 and 63 bytes",
        });
    }
    // Um Result<void> vazio representa sucesso.
    return {};
}

} // namespace

// Cria um Schema somente quando todas as colunas são válidas.
Result<Schema> Schema::create(std::vector<ColumnDefinition> columns) {
    // Uma tabela precisa possuir pelo menos uma coluna.
    if (columns.empty()) {
        return std::unexpected(Error{ErrorCode::empty_schema, "schema must have at least one column"});
    }
    // Impede schemas maiores que o limite definido para o produto.
    if (columns.size() > max_columns_per_table) {
        return std::unexpected(Error{
            ErrorCode::too_many_columns,
            "schema cannot contain more than " + std::to_string(max_columns_per_table) + " columns",
        });
    }

    // Guarda os nomes já visitados para detectar duplicidades rapidamente.
    std::unordered_set<std::string> names;
    // Examina cada coluna recebida.
    for (const auto& column : columns) {
        // Interrompe a criação se o nome atual for inválido.
        if (auto result = validate_column_name(column.name); !result) {
            return std::unexpected(result.error());
        }
        // insert retorna false quando o nome já estava no conjunto.
        if (!names.insert(column.name).second) {
            return std::unexpected(Error{
                ErrorCode::duplicate_column,
                "duplicate column: " + column.name,
            });
        }
    }

    // Move o vetor validado para dentro do novo Schema.
    return Schema{std::move(columns)};
}

// Converte uma lista entre chaves para vetor e reutiliza a validação principal.
Result<Schema> Schema::create(std::initializer_list<ColumnDefinition> columns) {
    return create(std::vector<ColumnDefinition>{columns});
}

// Informa quantas definições existem no vetor.
std::size_t Schema::column_count() const noexcept { return columns_.size(); }

// Retorna uma visão somente leitura de todas as definições.
std::span<const ColumnDefinition> Schema::columns() const noexcept { return columns_; }

// Procura uma coluna comparando seu nome.
Result<std::size_t> Schema::find_column(std::string_view name) const {
    // Percorre os índices para poder retornar a posição encontrada.
    for (std::size_t index = 0; index < columns_.size(); ++index) {
        // Encerra a busca assim que encontra o nome exato.
        if (columns_[index].name == name) {
            return index;
        }
    }
    // Informa que nenhuma coluna possui o nome solicitado.
    return std::unexpected(Error{
        ErrorCode::column_not_found,
        "column not found: " + std::string{name},
    });
}

// Confere se os valores de uma linha respeitam este Schema.
Result<void> Schema::validate(const Row& row) const {
    // Cada coluna precisa receber exatamente um valor.
    if (row.size() != columns_.size()) {
        // Monta uma mensagem que mostra as duas quantidades.
        std::ostringstream message;
        message << "row has " << row.size() << " values but schema requires " << columns_.size();
        return std::unexpected(Error{ErrorCode::value_count_mismatch, message.str()});
    }

    // Valida cada valor contra a coluna de mesmo índice.
    for (std::size_t index = 0; index < columns_.size(); ++index) {
        // Evita copiar a definição da coluna.
        const auto& column = columns_[index];
        // Evita copiar o valor da linha.
        const auto& value = row[index];
        // NULL possui uma regra própria de validação.
        if (value.is_null()) {
            // Rejeita NULL quando a coluna foi definida como NOT NULL.
            if (!column.nullable) {
                return std::unexpected(Error{
                    ErrorCode::null_constraint_violation,
                    "column does not accept NULL: " + column.name,
                });
            }
            // Não há tipo concreto para comparar quando o valor é NULL.
            continue;
        }

        // Rejeita um valor concreto de tipo diferente do tipo da coluna.
        if (value.data_type() != column.type) {
            return std::unexpected(Error{
                ErrorCode::type_mismatch,
                "invalid value type for column: " + column.name,
            });
        }
    }
    // Todas as colunas foram validadas com sucesso.
    return {};
}

} // namespace modb
