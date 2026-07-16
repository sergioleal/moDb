// Importa o limite de colunas validado neste teste.
#include "modb/limits.hpp"
// Importa os objetos relacionais usados no round-trip.
#include "modb/row.hpp"
#include "modb/schema.hpp"
#include "modb/value.hpp"
// Importa o leitor e o escritor de inteiros little-endian.
#include "modb/storage/binary.hpp"
// Importa os codecs de Value, Row e Schema.
#include "modb/storage/codec.hpp"

// Importa as funções simples de verificação.
#include "test_support.hpp"

// Disponibiliza std::equal para comparar as colunas.
#include <algorithm>
// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza limites usados no INTEGER mínimo.
#include <limits>
// Disponibiliza nomes únicos para o teste do limite de colunas.
#include <string>
// Disponibiliza visões de caracteres usadas na escrita manual.
#include <span>
// Disponibiliza std::string_view nos helpers.
#include <string_view>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza vetores de valores e bytes.
#include <vector>

namespace {

// Verifica se um Value sobrevive à codificação e à decodificação.
void check_value_round_trip(TestSuite& suite, const modb::Value& original) {
    // Converte o objeto para bytes.
    auto encoded = modb::storage::encode_value(original);
    // Confere se a codificação terminou sem erro.
    suite.check(encoded.has_value(), "Value is encoded");
    // Não tenta decodificar quando não existem bytes.
    if (!encoded) {
        return;
    }
    // Reconstrói um novo objeto a partir dos bytes.
    auto decoded = modb::storage::decode_value(*encoded);
    // Confere tipo e conteúdo do objeto reconstruído.
    suite.check(decoded.has_value() && *decoded == original, "Value survives round-trip");
}

// Acrescenta uma coluna codificada diretamente para criar entradas de teste.
void write_column(modb::storage::BinaryWriter& writer, std::string_view name,
                  std::uint8_t type, bool nullable) {
    // Grava o tamanho do nome em um byte.
    writer.write_u8(static_cast<std::uint8_t>(name.size()));
    // Converte os caracteres em uma visão de bytes.
    const auto characters = std::span<const char>{name.data(), name.size()};
    // Grava o nome sem terminador zero.
    writer.write_bytes(std::as_bytes(characters));
    // Grava a tag do tipo fornecida pelo cenário.
    writer.write_u8(type);
    // Grava zero ou um para nullable.
    writer.write_u8(nullable ? 1U : 0U);
}

} // namespace

// Executa todos os testes dos codecs binários.
int main() {
    // Evita repetir o namespace dos objetos relacionais.
    using namespace modb;
    // Evita repetir o namespace das classes de armazenamento.
    using namespace modb::storage;

    // Acumula e mostra as falhas encontradas.
    TestSuite suite;

    // Escreve uma sequência conhecida de inteiros.
    BinaryWriter primitive_writer;
    primitive_writer.write_u8(0x12U);
    primitive_writer.write_u16(0x3456U);
    primitive_writer.write_u32(0x789abcdeU);
    primitive_writer.write_u64(0x0123456789abcdefULL);
    // Lê a mesma sequência na mesma ordem.
    BinaryReader primitive_reader{primitive_writer.bytes()};
    suite.check(primitive_reader.read_u8() == Result<std::uint8_t>{0x12U},
                "u8 survives round-trip");
    suite.check(primitive_reader.read_u16() == Result<std::uint16_t>{0x3456U},
                "u16 survives little-endian round-trip");
    suite.check(primitive_reader.read_u32() == Result<std::uint32_t>{0x789abcdeU},
                "u32 survives little-endian round-trip");
    suite.check(primitive_reader.read_u64() == Result<std::uint64_t>{0x0123456789abcdefULL},
                "u64 survives little-endian round-trip");
    suite.check(primitive_reader.at_end(), "reader consumes all primitive bytes");
    suite.check_error(primitive_reader.read_u8(), ErrorCode::unexpected_end_of_input,
                      "reader rejects reads past the end");

    // Verifica cada representação suportada por Value.
    check_value_round_trip(suite, Value{Null{}});
    check_value_round_trip(suite, Value{false});
    check_value_round_trip(suite, Value{true});
    check_value_round_trip(suite, Value{std::numeric_limits<std::int64_t>::min()});
    check_value_round_trip(suite, Value{-12.5});
    check_value_round_trip(suite, Value{""});
    check_value_round_trip(suite, Value{"Ana"});

    // Monta uma linha contendo tipos diferentes.
    const Row original_row{Value{std::int64_t{1}}, Value{"Ana"}, Value{true}, Value{Null{}}};
    // Codifica a linha inteira.
    auto encoded_row = encode_row(original_row);
    suite.check(encoded_row.has_value(), "Row is encoded");
    // Decodifica somente quando a codificação funcionou.
    if (encoded_row) {
        auto decoded_row = decode_row(*encoded_row);
        suite.check(decoded_row.has_value() && *decoded_row == original_row,
                    "Row survives round-trip");

        // Remove o último byte para simular uma linha truncada.
        auto truncated_row = *encoded_row;
        truncated_row.pop_back();
        suite.check_error(decode_row(truncated_row), ErrorCode::unexpected_end_of_input,
                          "truncated Row is rejected");
    }

    // Uma linha forjada declara mais valores que o limite de colunas do modelo.
    // A contagem é rejeitada antes de qualquer reserva de memória, mesmo sem
    // nenhum Value depois do cabeçalho.
    BinaryWriter oversized_row_writer;
    oversized_row_writer.write_u16(static_cast<std::uint16_t>(max_columns_per_table + 1));
    suite.check_error(decode_row(oversized_row_writer.bytes()), ErrorCode::too_many_columns,
                      "Row declaring more values than the column limit is rejected");

    // O encode_row aplica o mesmo teto do decode_row, mantendo o round-trip simétrico.
    std::vector<Value> too_many_values(max_columns_per_table + 1, Value{std::int64_t{0}});
    suite.check_error(encode_row(Row{std::move(too_many_values)}), ErrorCode::too_many_columns,
                      "encode_row rejects a Row with more values than the column limit");

    // Cria um schema válido com todas as propriedades persistidas.
    auto original_schema = Schema::create({
        ColumnDefinition{"id", DataType::integer, false},
        ColumnDefinition{"name", DataType::text, true},
    });
    suite.check(original_schema.has_value(), "codec test schema is valid");
    // Codifica e decodifica o schema quando ele existe.
    if (original_schema) {
        auto encoded_schema = encode_schema(*original_schema);
        suite.check(encoded_schema.has_value(), "Schema is encoded");
        if (encoded_schema) {
            auto decoded_schema = decode_schema(*encoded_schema);
            const auto same_columns = decoded_schema &&
                                      std::equal(original_schema->columns().begin(),
                                                 original_schema->columns().end(),
                                                 decoded_schema->columns().begin(),
                                                 decoded_schema->columns().end());
            suite.check(same_columns, "Schema survives round-trip");
        }
    }

    // Uma tag que não pertence ao formato deve ser rejeitada.
    const std::vector invalid_tag_bytes{std::byte{0xffU}};
    suite.check_error(decode_value(invalid_tag_bytes), ErrorCode::invalid_encoding,
                      "unknown Value tag is rejected");

    // BOOLEAN aceita somente zero e um.
    const std::vector invalid_boolean{std::byte{1U}, std::byte{2U}};
    suite.check_error(decode_value(invalid_boolean), ErrorCode::invalid_encoding,
                      "invalid BOOLEAN byte is rejected");

    // Um objeto público não pode deixar bytes sem significado no final.
    auto encoded_null = encode_value(Value{Null{}});
    if (encoded_null) {
        encoded_null->push_back(std::byte{0U});
        suite.check_error(decode_value(*encoded_null), ErrorCode::trailing_data,
                          "trailing bytes are rejected");
    }

    // Declara um texto de cinco bytes, mas fornece somente dois.
    BinaryWriter short_text_writer;
    short_text_writer.write_u8(4U);
    short_text_writer.write_u32(5U);
    short_text_writer.write_u8(static_cast<std::uint8_t>('o'));
    short_text_writer.write_u8(static_cast<std::uint8_t>('i'));
    suite.check_error(decode_value(short_text_writer.bytes()),
                      ErrorCode::unexpected_end_of_input,
                      "TEXT shorter than its declared size is rejected");

    // Monta manualmente um schema com duas colunas chamadas id.
    BinaryWriter duplicate_schema_writer;
    duplicate_schema_writer.write_u16(2U);
    write_column(duplicate_schema_writer, "id", 2U, false);
    write_column(duplicate_schema_writer, "id", 2U, false);
    suite.check_error(decode_schema(duplicate_schema_writer.bytes()),
                      ErrorCode::duplicate_column,
                      "decoded Schema still validates duplicate columns");

    // Cria uma coluna a mais que o limite centralizado.
    std::vector<ColumnDefinition> too_many_columns;
    too_many_columns.reserve(max_columns_per_table + 1);
    for (std::size_t index = 0; index <= max_columns_per_table; ++index) {
        too_many_columns.push_back(
            ColumnDefinition{"column_" + std::to_string(index), DataType::integer, true});
    }
    suite.check_error(Schema::create(std::move(too_many_columns)), ErrorCode::too_many_columns,
                      "Schema rejects more than 256 columns");

    // Encerra o processo com o resultado acumulado.
    return suite.finish();
}
