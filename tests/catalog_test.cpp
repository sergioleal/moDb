// Importa o catálogo e os objetos usados em suas operações.
#include "modb/catalog.hpp"
#include "modb/row.hpp"
#include "modb/schema.hpp"
#include "modb/value.hpp"

// Importa as funções simples de verificação.
#include "test_support.hpp"

// Disponibiliza std::int64_t.
#include <cstdint>

// Executa os testes do catálogo em memória.
int main() {
    // Evita repetir modb:: em cada tipo deste teste.
    using namespace modb;

    // Acumula e mostra as falhas.
    TestSuite suite;
    // Cria o schema usado pela tabela users.
    auto schema = Schema::create({
        // id é obrigatório e inteiro.
        ColumnDefinition{"id", DataType::integer, false},
        // name é obrigatório e textual.
        ColumnDefinition{"name", DataType::text, false},
    });
    // Confere se a preparação do teste funcionou.
    suite.check(schema.has_value(), "test schema is valid");
    // Não executa cenários dependentes sem um schema válido.
    if (!schema) {
        return suite.finish();
    }

    // Cria um catálogo inicialmente vazio.
    Catalog catalog;
    // Adiciona a primeira tabela.
    suite.check(catalog.create_table("users", *schema).has_value(), "table is created");
    // Confere a rejeição do mesmo nome pela segunda vez.
    suite.check_error(catalog.create_table("users", *schema), ErrorCode::duplicate_table,
                      "duplicate table is rejected");
    // Confere a validação de um nome vazio.
    suite.check_error(catalog.create_table("", *schema), ErrorCode::invalid_identifier,
                      "empty table name is rejected");

    // Monta a linha válida que será inserida.
    const Row ana{Value{std::int64_t{1}}, Value{"Ana"}};
    // Confere a inserção na tabela existente.
    suite.check(catalog.insert("users", ana).has_value(), "valid row is inserted");
    // Confere que a tabela valida o tipo de cada valor.
    suite.check_error(catalog.insert("users", Row{Value{"wrong"}, Value{"Ana"}}),
                      ErrorCode::type_mismatch, "table validates rows before insertion");
    // Confere o erro ao inserir em uma tabela desconhecida.
    suite.check_error(catalog.insert("missing", ana), ErrorCode::table_not_found,
                      "insert into unknown table is rejected");

    // Solicita todas as linhas da tabela users.
    auto rows = catalog.scan("users");
    // Confere que o scan encontrou a tabela.
    suite.check(rows.has_value(), "existing table can be scanned");
    // Só examina o conteúdo quando o scan funcionou.
    if (rows) {
        // Inserções que falharam não devem acrescentar linhas.
        suite.check(rows->size() == 1, "failed insertions do not change the table");
        // A única linha precisa ser igual à linha inserida.
        suite.check((*rows)[0] == ana, "scan returns the inserted row");
    }
    // Confere o erro ao consultar uma tabela desconhecida.
    suite.check_error(catalog.scan("missing"), ErrorCode::table_not_found,
                      "unknown table cannot be scanned");

    // Encerra o processo com o resultado acumulado.
    return suite.finish();
}
