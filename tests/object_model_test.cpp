// Importa os tipos do modelo de objetos exercitados neste teste.
#include "modb/object/attribute_value.hpp"
#include "modb/object/baseline.hpp"
#include "modb/object/ids.hpp"
#include "modb/object/type_definition.hpp"
#include "modb/object/type_registry.hpp"

// Importa as funções simples de verificação dos testes.
#include "test_support.hpp"

// Disponibiliza std::byte usado pelo tipo BYTES.
#include <cstddef>
// Disponibiliza std::int64_t usado no valor inteiro.
#include <cstdint>
// Disponibiliza std::is_constructible_v para as garantias de construção.
#include <type_traits>
// Disponibiliza o contêiner usado para montar atributos e payloads.
#include <vector>

using namespace modb;
using namespace modb::object;

// AttributeValue aceita literais de texto, mas rejeita ponteiros arbitrários,
// pelo mesmo motivo que Value: evitar que um T*->bool acidental vire BOOLEAN.
static_assert(std::is_constructible_v<AttributeValue, const char*>,
              "AttributeValue must keep accepting string literals");
static_assert(!std::is_constructible_v<AttributeValue, int*>,
              "AttributeValue must reject arbitrary pointers");
static_assert(!std::is_constructible_v<AttributeValue, double*>,
              "AttributeValue must reject arbitrary pointers");

namespace {

// Testa construção, type() e o accessor correspondente de cada tag de valor.
void test_attribute_value_round_trip(TestSuite& suite) {
    suite.check(AttributeValue{}.is_null(), "default AttributeValue is null");
    suite.check(AttributeValue{}.type() == AttributeType::null,
                "default AttributeValue reports the null tag");

    suite.check(AttributeValue{true}.type() == AttributeType::boolean,
                "bool uses the BOOLEAN tag");
    suite.check(AttributeValue{true}.as_bool() == Result<bool>{true},
                "BOOLEAN round-trips through as_bool");

    suite.check(AttributeValue{std::int64_t{42}}.type() == AttributeType::int64,
                "signed integers use the INT64 tag");
    suite.check(AttributeValue{std::int64_t{42}}.as_int64() == Result<std::int64_t>{42},
                "INT64 round-trips through as_int64");

    suite.check(AttributeValue{3.5}.type() == AttributeType::float64,
                "double uses the FLOAT64 tag");
    suite.check(AttributeValue{3.5}.as_float64() == Result<double>{3.5},
                "FLOAT64 round-trips through as_float64");

    suite.check(AttributeValue{"Ana"}.type() == AttributeType::string,
                "string literals use the STRING tag");
    suite.check(AttributeValue{"Ana"}.as_string() == Result<std::string_view>{"Ana"},
                "STRING round-trips through as_string");

    const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0xff}};
    suite.check(AttributeValue{bytes}.type() == AttributeType::bytes,
                "byte vectors use the BYTES tag");
    // Mantido em uma variável: o span de as_bytes() aponta para dentro deste
    // AttributeValue, que precisa sobreviver até a checagem ser avaliada.
    const AttributeValue byte_value{bytes};
    auto read_bytes = byte_value.as_bytes();
    suite.check(read_bytes.has_value() && read_bytes->size() == 2 &&
                    (*read_bytes)[0] == std::byte{0x01} && (*read_bytes)[1] == std::byte{0xff},
                "BYTES round-trips through as_bytes");

    suite.check(AttributeValue{ObjectId{7}}.type() == AttributeType::ref,
                "ObjectId uses the REF tag");
    suite.check(AttributeValue{ObjectId{7}}.as_ref() == Result<ObjectId>{ObjectId{7}},
                "REF round-trips through as_ref");

    suite.check(AttributeValue{BlobId{9}}.type() == AttributeType::blob,
                "BlobId uses the BLOB tag");
    suite.check(AttributeValue{BlobId{9}}.as_blob() == Result<BlobId>{BlobId{9}},
                "BLOB round-trips through as_blob");

    // Um accessor chamado sobre a tag errada retorna erro, nunca lança.
    suite.check_error(AttributeValue{"Ana"}.as_int64(), ErrorCode::type_mismatch,
                      "wrong accessor is rejected with type_mismatch");
}

// Monta um AttributeDefinition simples com os campos mais usados.
AttributeDefinition make_attribute(std::uint16_t id, std::string name, AttributeType type,
                                   bool nullable = true) {
    return AttributeDefinition{.id = FieldId{id}, .name = std::move(name), .type = type,
                               .nullable = nullable};
}

// Testa a validação estrutural de TypeDefinition::create.
void test_type_definition_validation(TestSuite& suite) {
    // Uma definição bem formada é aceita.
    auto employee = TypeDefinition::create(
        "Employee", std::vector<AttributeDefinition>{
                        make_attribute(1, "name", AttributeType::string, false),
                        make_attribute(2, "salary", AttributeType::float64, false),
                    });
    suite.check(employee.has_value(), "well-formed type definition is accepted");
    if (employee) {
        suite.check(employee->id() == TypeDefinitionId{0},
                    "a freshly created type has no id yet");
        suite.check(employee->find(FieldId{1})->name == "name",
                    "attributes are found by FieldId");
        suite.check(employee->find("salary")->id == FieldId{2},
                    "attributes are found by name");
        suite.check(employee->find(FieldId{99}) == nullptr,
                    "unknown FieldId resolves to nullptr");
    }

    // Um tipo sem atributos é degenerado, mas válido (ao contrário do Schema
    // relacional, que exige ao menos uma coluna).
    suite.check(TypeDefinition::create("Marker", {}).has_value(),
                "a type without attributes is valid");

    // FieldId duplicado.
    suite.check_error(
        TypeDefinition::create("Bad",
                               {make_attribute(1, "a", AttributeType::int64),
                                make_attribute(1, "b", AttributeType::int64)}),
        ErrorCode::duplicate_field, "duplicate FieldId is rejected");

    // FieldId zero.
    suite.check_error(
        TypeDefinition::create("Bad", {make_attribute(0, "a", AttributeType::int64)}),
        ErrorCode::invalid_argument, "FieldId zero is rejected");

    // Nome de atributo duplicado.
    suite.check_error(
        TypeDefinition::create("Bad",
                               {make_attribute(1, "a", AttributeType::int64),
                                make_attribute(2, "a", AttributeType::int64)}),
        ErrorCode::duplicate_column, "duplicate attribute name is rejected");

    // Mais atributos que o limite do produto.
    std::vector<AttributeDefinition> too_many;
    too_many.reserve(257);
    for (std::uint16_t index = 1; index <= 257; ++index) {
        too_many.push_back(make_attribute(index, "f" + std::to_string(index), AttributeType::int64));
    }
    suite.check_error(TypeDefinition::create("Bad", std::move(too_many)),
                      ErrorCode::too_many_columns, "more than 256 attributes is rejected");

    // Nome de tipo maior que o limite de identificador.
    suite.check_error(TypeDefinition::create(std::string(64, 'x'), {}),
                      ErrorCode::invalid_identifier, "type name over 63 bytes is rejected");

    // Default incompatível com o tipo do atributo.
    auto mismatched_default = make_attribute(1, "salary", AttributeType::float64, false);
    mismatched_default.default_value = AttributeValue{"not a number"};
    suite.check_error(TypeDefinition::create("Bad", {mismatched_default}),
                      ErrorCode::type_mismatch, "default value type mismatch is rejected");

    // Default null num atributo não-nullable.
    auto null_default_not_nullable = make_attribute(1, "salary", AttributeType::float64, false);
    null_default_not_nullable.default_value = AttributeValue{};
    suite.check_error(TypeDefinition::create("Bad", {null_default_not_nullable}),
                      ErrorCode::null_constraint_violation,
                      "null default on a non-nullable attribute is rejected");

    // Default num atributo embutido não faz sentido nesta fase.
    auto embedded_with_default = make_attribute(1, "address", AttributeType::string);
    embedded_with_default.is_embedded = true;
    embedded_with_default.default_value = AttributeValue{"x"};
    suite.check_error(TypeDefinition::create("Bad", {embedded_with_default}),
                      ErrorCode::invalid_argument,
                      "default value on an embedded attribute is rejected");
}

// Testa validate_object contra uma TypeDefinition fixa.
void test_validate_object(TestSuite& suite) {
    auto with_default = make_attribute(3, "country", AttributeType::string, true);
    with_default.default_value = AttributeValue{"BR"};

    auto employee = TypeDefinition::create(
        "Employee", std::vector<AttributeDefinition>{
                        make_attribute(1, "name", AttributeType::string, false),
                        make_attribute(2, "salary", AttributeType::float64, false),
                        with_default,
                    });
    suite.check(employee.has_value(), "fixture type definition is valid");
    if (!employee) {
        return;
    }

    // Um payload completo e compatível é aceito.
    const FieldValues complete{
        {FieldId{1}, AttributeValue{"Ana"}},
        {FieldId{2}, AttributeValue{15000.0}},
        {FieldId{3}, AttributeValue{"US"}},
    };
    suite.check(validate_object(*employee, complete).has_value(),
                "a complete, compatible payload is accepted");

    // Um atributo ausente que possui default é aceito.
    const FieldValues missing_with_default{
        {FieldId{1}, AttributeValue{"Ana"}},
        {FieldId{2}, AttributeValue{15000.0}},
    };
    suite.check(validate_object(*employee, missing_with_default).has_value(),
                "a missing attribute with a default is accepted");

    // Um atributo obrigatório ausente e sem default é rejeitado.
    const FieldValues missing_required{
        {FieldId{2}, AttributeValue{15000.0}},
    };
    suite.check_error(validate_object(*employee, missing_required),
                      ErrorCode::null_constraint_violation,
                      "a missing, non-nullable, default-less attribute is rejected");

    // null explícito num atributo não-nullable é rejeitado.
    const FieldValues explicit_null{
        {FieldId{1}, AttributeValue{}},
        {FieldId{2}, AttributeValue{15000.0}},
    };
    suite.check_error(validate_object(*employee, explicit_null),
                      ErrorCode::null_constraint_violation,
                      "explicit null on a non-nullable attribute is rejected");

    // FieldId que não existe no tipo.
    const FieldValues unknown_field{
        {FieldId{1}, AttributeValue{"Ana"}},
        {FieldId{2}, AttributeValue{15000.0}},
        {FieldId{99}, AttributeValue{"?"}},
    };
    suite.check_error(validate_object(*employee, unknown_field), ErrorCode::field_not_found,
                      "an unknown FieldId is rejected");

    // FieldId duplicado dentro do próprio payload.
    const FieldValues duplicate_field{
        {FieldId{1}, AttributeValue{"Ana"}},
        {FieldId{1}, AttributeValue{"Beatriz"}},
        {FieldId{2}, AttributeValue{15000.0}},
    };
    suite.check_error(validate_object(*employee, duplicate_field), ErrorCode::duplicate_field,
                      "a duplicate FieldId in the payload is rejected");

    // Tipo de valor incompatível com o atributo.
    const FieldValues wrong_type{
        {FieldId{1}, AttributeValue{std::int64_t{1}}},
        {FieldId{2}, AttributeValue{15000.0}},
    };
    suite.check_error(validate_object(*employee, wrong_type), ErrorCode::type_mismatch,
                      "a value of the wrong type is rejected");
}

// Testa TypeRegistry: atribuição de id, unicidade de nome e as duas buscas.
void test_type_registry(TestSuite& suite) {
    TypeRegistry registry;

    auto employee = TypeDefinition::create(
        "Employee", std::vector<AttributeDefinition>{
                        make_attribute(1, "name", AttributeType::string, false),
                    });
    suite.check(employee.has_value(), "fixture type definition is valid");
    if (!employee) {
        return;
    }

    auto first_id = registry.register_type(*employee);
    suite.check(first_id.has_value(), "a new type name is registered");
    suite.check(first_id.has_value() && first_id->value == first_user_object_id,
                "the first registered type receives the first user ObjectId");

    // Um segundo tipo com o mesmo nome é rejeitado.
    suite.check_error(registry.register_type(*employee), ErrorCode::duplicate_type,
                      "registering the same type name twice is rejected");

    if (first_id) {
        auto by_id = registry.find(*first_id);
        suite.check(by_id.has_value() && by_id->get().name() == "Employee",
                    "a registered type is found by id");
    }
    auto by_name = registry.find("Employee");
    suite.check(by_name.has_value() && by_name->get().id() == first_id,
                "a registered type is found by name and carries the assigned id");

    suite.check_error(registry.find(TypeDefinitionId{999}), ErrorCode::type_not_found,
                      "an unknown id is reported");
    suite.check_error(registry.find("Unknown"), ErrorCode::type_not_found,
                      "an unknown name is reported");

    // Um segundo tipo distinto recebe o próximo id da sequência.
    auto department = TypeDefinition::create(
        "Department", std::vector<AttributeDefinition>{
                          make_attribute(1, "name", AttributeType::string, false),
                      });
    if (department) {
        auto second_id = registry.register_type(*department);
        suite.check(second_id.has_value() && first_id.has_value() &&
                        second_id->value == first_id->value + 1,
                    "distinct types receive monotonically increasing ids");
    }
}

// Testa Baseline: criação válida, vazia, e as duas rejeições de conteúdo.
void test_baseline(TestSuite& suite) {
    auto baseline =
        Baseline::create(std::vector<TypeDefinitionId>{TypeDefinitionId{16}, TypeDefinitionId{17}});
    suite.check(baseline.has_value(), "a baseline with valid type ids is accepted");
    if (baseline) {
        suite.check(baseline->id() == BaselineId{0}, "a freshly created baseline has no id yet");
        suite.check(baseline->types().size() == 2, "the baseline preserves all type ids");
    }

    suite.check(Baseline::create({}).has_value(),
                "an empty baseline is valid (catalog before any user type)");

    suite.check_error(Baseline::create({TypeDefinitionId{16}, TypeDefinitionId{16}}),
                      ErrorCode::duplicate_type, "a repeated TypeDefinitionId is rejected");

    suite.check_error(Baseline::create({TypeDefinitionId{0}}), ErrorCode::invalid_object_id,
                      "TypeDefinitionId zero is rejected");
}

} // namespace

// Executa os testes do modelo de objetos.
int main() {
    TestSuite suite;

    test_attribute_value_round_trip(suite);
    test_type_definition_validation(suite);
    test_validate_object(suite);
    test_type_registry(suite);
    test_baseline(suite);

    return suite.finish();
}
