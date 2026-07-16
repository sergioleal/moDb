// Importa a declaração de Catalog.
#include "modb/catalog.hpp"

// Importa o limite compartilhado para nomes de tabelas e colunas.
#include "modb/limits.hpp"

// Disponibiliza std::move para transferir tabelas e linhas.
#include <utility>

namespace modb {
namespace {

// Valida apenas o tamanho do nome de uma tabela.
Result<void> validate_table_name(std::string_view name) {
    // Rejeita nomes vazios ou maiores que 63 bytes.
    if (name.empty() || name.size() > max_identifier_bytes) {
        return std::unexpected(Error{
            ErrorCode::invalid_identifier,
            "table name must contain between 1 and 63 bytes",
        });
    }
    // O nome respeita as regras atuais.
    return {};
}

} // namespace

// Cria e registra uma nova tabela no catálogo.
Result<void> Catalog::create_table(std::string name, Schema schema) {
    // Não continua quando o identificador é inválido.
    if (auto result = validate_table_name(name); !result) {
        return result;
    }
    // contains verifica se o mapa já possui essa chave.
    if (tables_.contains(name)) {
        return std::unexpected(Error{
            ErrorCode::duplicate_table,
            "table already exists: " + name,
        });
    }

    // Mantém uma cópia do nome para o objeto Table antes de mover a chave.
    auto table_name = name;
    // Insere a chave e a tabela no mapa transferindo seus recursos.
    tables_.emplace(std::move(name), Table{std::move(table_name), std::move(schema)});
    // Informa que a tabela foi criada.
    return {};
}

// Encaminha uma linha para a tabela indicada.
Result<void> Catalog::insert(std::string_view table_name, Row row) {
    // Procura a tabela com permissão de alteração.
    auto found = mutable_table(table_name);
    // Propaga o erro se a tabela não existir.
    if (!found) {
        return std::unexpected(found.error());
    }
    // Obtém a Table da referência e move a linha para sua inserção.
    return found->get().insert(std::move(row));
}

// Procura uma tabela para leitura.
Result<std::reference_wrapper<const Table>> Catalog::table(std::string_view name) const {
    // Converte a visão para string porque este mapa usa std::string como chave.
    const auto iterator = tables_.find(std::string{name});
    // end indica que nenhuma entrada foi encontrada.
    if (iterator == tables_.end()) {
        return std::unexpected(Error{
            ErrorCode::table_not_found,
            "table not found: " + std::string{name},
        });
    }
    // std::cref retorna uma referência constante que cabe dentro de Result.
    return std::cref(iterator->second);
}

// Retorna uma visão das linhas de uma tabela.
Result<std::span<const Row>> Catalog::scan(std::string_view table_name) const {
    // Procura a tabela somente para leitura.
    auto found = table(table_name);
    // Propaga o erro se a tabela não existir.
    if (!found) {
        return std::unexpected(found.error());
    }
    // Obtém a Table da referência e retorna sua visão de linhas.
    return found->get().scan();
}

// Procura uma tabela permitindo alterações internas.
Result<std::reference_wrapper<Table>> Catalog::mutable_table(std::string_view name) {
    // Procura a chave no mapa.
    const auto iterator = tables_.find(std::string{name});
    // Retorna erro quando a tabela não existe.
    if (iterator == tables_.end()) {
        return std::unexpected(Error{
            ErrorCode::table_not_found,
            "table not found: " + std::string{name},
        });
    }
    // std::ref permite transportar uma referência modificável no Result.
    return std::ref(iterator->second);
}

} // namespace modb
