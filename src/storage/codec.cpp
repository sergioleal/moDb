// Importa as funções públicas dos codecs.
#include "modb/storage/codec.hpp"

// Importa o leitor e o escritor binários little-endian.
#include "modb/storage/binary.hpp"
// Importa os limites compartilhados do modelo.
#include "modb/limits.hpp"
// Importa o helper de visitação exaustiva de variant.
#include "modb/detail/overloaded.hpp"

// Disponibiliza std::bit_cast para preservar os bits de double.
#include <bit>
// Disponibiliza inteiros com largura fixa.
#include <cstdint>
// Disponibiliza limites máximos dos campos de tamanho.
#include <limits>
// Disponibiliza o armazenamento de textos decodificados.
#include <string>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza std::variant e std::visit.
#include <variant>
// Disponibiliza vetores de valores e colunas.
#include <vector>

namespace modb::storage {
namespace {

// Define tags estáveis que são gravadas no formato binário.
enum class ValueTag : std::uint8_t {
    // Representa NULL sem bytes adicionais.
    null_value = 0,
    // Representa BOOLEAN seguido por um byte.
    boolean = 1,
    // Representa INTEGER seguido por oito bytes.
    integer = 2,
    // Representa REAL seguido por oito bytes.
    real = 3,
    // Representa TEXT seguido por tamanho e conteúdo.
    text = 4,
};

// Cria um erro para uma tag que não pertence ao formato.
Error invalid_tag(std::uint8_t tag) {
    // Inclui o número recebido para facilitar o diagnóstico.
    return Error{ErrorCode::invalid_encoding, "invalid type tag: " + std::to_string(tag)};
}

// Cria um erro quando restam bytes depois do objeto completo.
Error trailing_bytes(std::size_t count) {
    // Informa quantos bytes não foram consumidos.
    return Error{
        ErrorCode::trailing_data,
        "encoded object has " + std::to_string(count) + " trailing bytes",
    };
}

// Converte DataType para a mesma tag estável usada por Value.
std::uint8_t encode_data_type(DataType type) {
    // Escolhe uma tag explícita sem depender do valor interno do enum.
    switch (type) {
    case DataType::boolean:
        return static_cast<std::uint8_t>(ValueTag::boolean);
    case DataType::integer:
        return static_cast<std::uint8_t>(ValueTag::integer);
    case DataType::real:
        return static_cast<std::uint8_t>(ValueTag::real);
    case DataType::text:
        return static_cast<std::uint8_t>(ValueTag::text);
    }
    // O código só chega aqui se DataType possuir um valor inválido.
    return 0xffU;
}

// Converte uma tag persistida para DataType.
Result<DataType> decode_data_type(std::uint8_t tag) {
    // Compara a tag com cada valor permitido para colunas.
    switch (static_cast<ValueTag>(tag)) {
    case ValueTag::boolean:
        return DataType::boolean;
    case ValueTag::integer:
        return DataType::integer;
    case ValueTag::real:
        return DataType::real;
    case ValueTag::text:
        return DataType::text;
    case ValueTag::null_value:
        // NULL é um valor, mas não pode ser o tipo declarado de uma coluna.
        break;
    }
    // Rejeita NULL e qualquer número não reconhecido.
    return std::unexpected(invalid_tag(tag));
}

// Acrescenta um Value a um escritor que pode conter outros objetos.
Result<void> encode_value_to(BinaryWriter& writer, const Value& value) {
    // std::visit torna a cobertura das alternativas verificada em compilação:
    // uma alternativa nova em Value::Storage sem caso aqui deixa de compilar.
    return std::visit(
        detail::Overloaded{
            // NULL precisa somente de sua tag.
            [&](Null) -> Result<void> {
                writer.write_u8(static_cast<std::uint8_t>(ValueTag::null_value));
                return {};
            },
            // BOOLEAN usa a tag um e um byte zero ou um.
            [&](bool boolean) -> Result<void> {
                writer.write_u8(static_cast<std::uint8_t>(ValueTag::boolean));
                writer.write_u8(boolean ? 1U : 0U);
                return {};
            },
            // INTEGER usa a tag dois e preserva os 64 bits do inteiro com sinal.
            [&](std::int64_t integer) -> Result<void> {
                writer.write_u8(static_cast<std::uint8_t>(ValueTag::integer));
                writer.write_u64(std::bit_cast<std::uint64_t>(integer));
                return {};
            },
            // REAL usa a tag três e preserva exatamente os bits IEEE 754 do double.
            [&](double real) -> Result<void> {
                writer.write_u8(static_cast<std::uint8_t>(ValueTag::real));
                writer.write_u64(std::bit_cast<std::uint64_t>(real));
                return {};
            },
            // TEXT usa a tag quatro, precedida do tamanho em uint32_t.
            [&](const std::string& text) -> Result<void> {
                if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected(
                        Error{ErrorCode::value_too_large, "text is too large to encode"});
                }
                writer.write_u8(static_cast<std::uint8_t>(ValueTag::text));
                writer.write_u32(static_cast<std::uint32_t>(text.size()));
                const auto characters = std::span<const char>{text.data(), text.size()};
                writer.write_bytes(std::as_bytes(characters));
                return {};
            },
        },
        value.storage());
}

// Lê um Value da posição atual e deixa o leitor após seu último byte.
Result<Value> decode_value_from(BinaryReader& reader) {
    // Todo Value começa por uma tag de um byte.
    auto encoded_tag = reader.read_u8();
    // Propaga uma entrada vazia ou truncada.
    if (!encoded_tag) {
        return std::unexpected(encoded_tag.error());
    }

    // Interpreta os bytes seguintes de acordo com a tag.
    switch (static_cast<ValueTag>(*encoded_tag)) {
    case ValueTag::null_value:
        // NULL não possui conteúdo adicional.
        return Value{Null{}};
    case ValueTag::boolean: {
        // BOOLEAN precisa de um byte de conteúdo.
        auto boolean = reader.read_u8();
        // Propaga uma entrada truncada.
        if (!boolean) {
            return std::unexpected(boolean.error());
        }
        // Somente zero e um são representações válidas.
        if (*boolean > 1U) {
            return std::unexpected(
                Error{ErrorCode::invalid_encoding, "BOOLEAN must be encoded as 0 or 1"});
        }
        // Converte um em true e zero em false.
        return Value{*boolean == 1U};
    }
    case ValueTag::integer: {
        // Lê os oito bytes do INTEGER.
        auto integer = reader.read_u64();
        // Propaga uma entrada truncada.
        if (!integer) {
            return std::unexpected(integer.error());
        }
        // Reinterpreta os mesmos bits como inteiro com sinal.
        return Value{std::bit_cast<std::int64_t>(*integer)};
    }
    case ValueTag::real: {
        // Lê os oito bytes do REAL.
        auto real = reader.read_u64();
        // Propaga uma entrada truncada.
        if (!real) {
            return std::unexpected(real.error());
        }
        // Reinterpreta os mesmos bits como double.
        return Value{std::bit_cast<double>(*real)};
    }
    case ValueTag::text: {
        // Lê o tamanho de 32 bits do texto.
        auto size = reader.read_u32();
        // Propaga um campo de tamanho truncado.
        if (!size) {
            return std::unexpected(size.error());
        }
        // Lê exatamente a quantidade de bytes declarada.
        auto text = reader.read_bytes(*size);
        // Propaga um conteúdo truncado.
        if (!text) {
            return std::unexpected(text.error());
        }
        // Copia os bytes para uma string que possui sua memória.
        return Value{std::string{
            reinterpret_cast<const char*>(text->data()),
            text->size(),
        }};
    }
    }
    // Qualquer outra tag é inválida.
    return std::unexpected(invalid_tag(*encoded_tag));
}

// Exige que um objeto público ocupe exatamente toda a entrada.
Result<void> require_end(const BinaryReader& reader) {
    // Bytes restantes podem esconder uma codificação incorreta.
    if (!reader.at_end()) {
        return std::unexpected(trailing_bytes(reader.remaining()));
    }
    // A entrada terminou junto com o objeto.
    return {};
}

} // namespace

// Codifica um Value independente.
Result<std::vector<std::byte>> encode_value(const Value& value) {
    // Cria um vetor binário inicialmente vazio.
    BinaryWriter writer;
    // Acrescenta tag e conteúdo do Value.
    if (auto result = encode_value_to(writer, value); !result) {
        return std::unexpected(result.error());
    }
    // Transfere o vetor pronto para o chamador.
    return std::move(writer).take();
}

// Decodifica um Value independente.
Result<Value> decode_value(std::span<const std::byte> bytes) {
    // Começa a leitura no primeiro byte.
    BinaryReader reader{bytes};
    // Reconstrói tag e conteúdo.
    auto value = decode_value_from(reader);
    // Propaga qualquer codificação inválida.
    if (!value) {
        return std::unexpected(value.error());
    }
    // Rejeita conteúdo depois do Value.
    if (auto result = require_end(reader); !result) {
        return std::unexpected(result.error());
    }
    // Retorna o objeto reconstruído.
    return value;
}

// Codifica uma linha independente.
Result<std::vector<std::byte>> encode_row(const Row& row) {
    // Uma linha nunca pode ter mais valores que o limite de colunas do modelo.
    // Manter o mesmo teto do decode_row garante o round-trip e cabe em uint16_t.
    if (row.size() > max_columns_per_table) {
        return std::unexpected(Error{ErrorCode::too_many_columns, "row has too many values"});
    }
    // Cria o escritor que receberá a linha completa.
    BinaryWriter writer;
    // Grava quantos Values vêm em seguida.
    writer.write_u16(static_cast<std::uint16_t>(row.size()));
    // Codifica os Values na ordem das colunas.
    for (const auto& value : row.values()) {
        if (auto result = encode_value_to(writer, value); !result) {
            return std::unexpected(result.error());
        }
    }
    // Transfere os bytes prontos.
    return std::move(writer).take();
}

// Decodifica uma linha independente.
Result<Row> decode_row(std::span<const std::byte> bytes) {
    // Começa a leitura no primeiro byte.
    BinaryReader reader{bytes};
    // Lê quantos Values devem ser reconstruídos.
    auto count = reader.read_u16();
    // Propaga uma entrada truncada.
    if (!count) {
        return std::unexpected(count.error());
    }
    // Rejeita a quantidade antes de reservar memória, espelhando decode_schema.
    // Uma contagem forjada (até 65535) não pode mais provocar uma alocação enorme,
    // e uma linha com mais valores que colunas possíveis é estruturalmente inválida.
    if (*count > max_columns_per_table) {
        return std::unexpected(Error{ErrorCode::too_many_columns, "row has too many values"});
    }
    // Reserva a capacidade para evitar realocações durante o loop.
    std::vector<Value> values;
    values.reserve(*count);
    // Decodifica exatamente a quantidade declarada.
    for (std::uint16_t index = 0; index < *count; ++index) {
        auto value = decode_value_from(reader);
        if (!value) {
            return std::unexpected(value.error());
        }
        values.push_back(std::move(*value));
    }
    // Rejeita bytes não pertencentes à linha.
    if (auto result = require_end(reader); !result) {
        return std::unexpected(result.error());
    }
    // Move o vetor reconstruído para a Row.
    return Row{std::move(values)};
}

// Codifica um schema independente.
Result<std::vector<std::byte>> encode_schema(const Schema& schema) {
    // Um schema válido nunca deve ultrapassar o limite compartilhado.
    if (schema.column_count() > max_columns_per_table) {
        return std::unexpected(Error{ErrorCode::too_many_columns, "schema has too many columns"});
    }
    // Cria o escritor que receberá o schema completo.
    BinaryWriter writer;
    // Grava quantas definições vêm em seguida.
    writer.write_u16(static_cast<std::uint16_t>(schema.column_count()));
    // Codifica cada definição na ordem original.
    for (const auto& column : schema.columns()) {
        // O limite de identificador garante que o tamanho cabe em uint8_t.
        writer.write_u8(static_cast<std::uint8_t>(column.name.size()));
        // Converte os caracteres do nome em uma visão de bytes.
        const auto characters = std::span<const char>{column.name.data(), column.name.size()};
        // Grava o nome sem terminador zero.
        writer.write_bytes(std::as_bytes(characters));
        // Grava uma tag explícita para o tipo da coluna.
        writer.write_u8(encode_data_type(column.type));
        // Grava zero ou um para a propriedade nullable.
        writer.write_u8(column.nullable ? 1U : 0U);
    }
    // Transfere os bytes prontos.
    return std::move(writer).take();
}

// Decodifica e valida um schema independente.
Result<Schema> decode_schema(std::span<const std::byte> bytes) {
    // Começa a leitura no primeiro byte.
    BinaryReader reader{bytes};
    // Lê a quantidade de definições.
    auto count = reader.read_u16();
    // Propaga uma entrada truncada.
    if (!count) {
        return std::unexpected(count.error());
    }
    // Rejeita a quantidade antes de reservar memória.
    if (*count > max_columns_per_table) {
        return std::unexpected(Error{ErrorCode::too_many_columns, "schema has too many columns"});
    }

    // Reserva espaço para todas as colunas declaradas.
    std::vector<ColumnDefinition> columns;
    columns.reserve(*count);
    // Reconstrói cada definição.
    for (std::uint16_t index = 0; index < *count; ++index) {
        // Lê o tamanho do nome.
        auto name_size = reader.read_u8();
        if (!name_size) {
            return std::unexpected(name_size.error());
        }
        // Lê os bytes pertencentes ao nome.
        auto name_bytes = reader.read_bytes(*name_size);
        if (!name_bytes) {
            return std::unexpected(name_bytes.error());
        }
        // Copia o nome para uma string própria.
        std::string name{
            reinterpret_cast<const char*>(name_bytes->data()),
            name_bytes->size(),
        };
        // Lê a tag do tipo da coluna.
        auto encoded_type = reader.read_u8();
        if (!encoded_type) {
            return std::unexpected(encoded_type.error());
        }
        // Converte e valida a tag.
        auto type = decode_data_type(*encoded_type);
        if (!type) {
            return std::unexpected(type.error());
        }
        // Lê a propriedade nullable.
        auto nullable = reader.read_u8();
        if (!nullable) {
            return std::unexpected(nullable.error());
        }
        // Somente zero e um são permitidos no formato.
        if (*nullable > 1U) {
            return std::unexpected(
                Error{ErrorCode::invalid_encoding, "nullable must be encoded as 0 or 1"});
        }
        // Acrescenta a definição reconstruída.
        columns.push_back(ColumnDefinition{std::move(name), *type, *nullable == 1U});
    }
    // Rejeita bytes não pertencentes ao schema.
    if (auto result = require_end(reader); !result) {
        return std::unexpected(result.error());
    }
    // Reutiliza as validações de nomes, duplicidade e quantidade.
    return Schema::create(std::move(columns));
}

} // namespace modb::storage

