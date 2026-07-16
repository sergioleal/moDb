// Importa os tipos relacionais exercitados neste teste.
#include "modb/data_type.hpp"
#include "modb/row.hpp"
#include "modb/schema.hpp"
#include "modb/value.hpp"

// Importa as funções simples de verificação dos testes.
#include "test_support.hpp"

// Disponibiliza std::int64_t usado no valor inteiro.
#include <cstdint>
// Disponibiliza std::is_constructible_v para as garantias de construção de Value.
#include <type_traits>

// Value aceita literais de texto, mas rejeita ponteiros arbitrários (evita que
// um T*->bool acidental vire BOOLEAN). Garantido em tempo de compilação.
static_assert(std::is_constructible_v<modb::Value, const char*>,
              "Value must keep accepting string literals");
static_assert(!std::is_constructible_v<modb::Value, int*>,
              "Value must reject arbitrary pointers");
static_assert(!std::is_constructible_v<modb::Value, double*>,
              "Value must reject arbitrary pointers");

// Executa os testes do modelo de dados.
int main() {
    // Evita repetir modb:: em cada tipo deste teste.
    using namespace modb;

    // Acumula e imprime as falhas encontradas.
    TestSuite suite;
    // Confere o texto SQL associado ao tipo inteiro.
    suite.check(data_type_name(DataType::integer) == "INTEGER", "INTEGER has a stable name");
    // Confere que um Value sem argumento começa como NULL.
    suite.check(Value{}.is_null(), "default Value is NULL");
    // Confere a dedução do tipo para inteiros com sinal.
    suite.check(Value{42}.data_type() == DataType::integer, "signed integers use INTEGER");
    // Confere a dedução do tipo para literais de texto.
    suite.check(Value{"Ana"}.data_type() == DataType::text, "string literals use TEXT");

    // Tenta criar um schema válido para os próximos cenários.
    auto schema = Schema::create({
        // id é um INTEGER obrigatório.
        ColumnDefinition{"id", DataType::integer, false},
        // name é um TEXT obrigatório.
        ColumnDefinition{"name", DataType::text, false},
        // active é um BOOLEAN que aceita NULL.
        ColumnDefinition{"active", DataType::boolean, true},
    });
    // Confere se a criação do schema funcionou.
    suite.check(schema.has_value(), "valid schema is accepted");
    // Interrompe os testes dependentes caso o schema não exista.
    if (!schema) {
        return suite.finish();
    }

    // Confere se nenhuma coluna foi perdida.
    suite.check(schema->column_count() == 3, "schema keeps all columns");
    // Confere se a busca retorna o índice da segunda coluna.
    suite.check(schema->find_column("name") == Result<std::size_t>{1},
                "columns are resolved by name");
    // Confere o erro produzido para uma coluna inexistente.
    suite.check_error(schema->find_column("missing"), ErrorCode::column_not_found,
                      "unknown columns are rejected");

    // Monta uma linha que respeita quantidade, tipos e nulabilidade.
    const Row valid{Value{std::int64_t{1}}, Value{"Ana"}, Value{Null{}}};
    // Confere se a linha válida é aceita.
    suite.check(schema->validate(valid).has_value(), "valid row matches schema");

    // Confere a rejeição de uma linha com poucos valores.
    suite.check_error(schema->validate(Row{Value{1}, Value{"Ana"}}),
                      ErrorCode::value_count_mismatch, "wrong value count is rejected");
    // Confere a rejeição de texto na coluna id.
    suite.check_error(schema->validate(Row{Value{"wrong"}, Value{"Ana"}, Value{true}}),
                      ErrorCode::type_mismatch, "wrong value type is rejected");
    // Confere a rejeição de NULL na coluna id obrigatória.
    suite.check_error(schema->validate(Row{Value{Null{}}, Value{"Ana"}, Value{true}}),
                      ErrorCode::null_constraint_violation,
                      "NULL in a NOT NULL column is rejected");

    // Confere que um schema precisa possuir colunas.
    suite.check_error(Schema::create({}), ErrorCode::empty_schema, "empty schema is rejected");
    // Confere que duas colunas não podem usar o mesmo nome.
    suite.check_error(
        Schema::create({
            ColumnDefinition{"id", DataType::integer},
            ColumnDefinition{"id", DataType::integer},
        }),
        ErrorCode::duplicate_column, "duplicate columns are rejected");

    // Encerra o processo com o resultado acumulado.
    return suite.finish();
}
