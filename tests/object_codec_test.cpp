// Importa o codec exercitado neste teste.
#include "modb/object/object_codec.hpp"

// Importa as funções simples de verificação dos testes.
#include "test_support.hpp"

// Disponibiliza std::byte usado ao montar payloads maliciosos.
#include <cstddef>
// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza o contêiner dos campos e dos payloads de teste.
#include <vector>

using namespace modb;
using namespace modb::object;

namespace {

// Faz um round-trip completo de um objeto e confere igualdade.
void test_round_trip_all_types(TestSuite& suite) {
    const FieldValues fields{
        {FieldId{1}, AttributeValue{AttributeNull{}}},
        {FieldId{2}, AttributeValue{true}},
        {FieldId{3}, AttributeValue{std::int64_t{-42}}},
        {FieldId{4}, AttributeValue{3.5}},
        {FieldId{5}, AttributeValue{"Ana"}},
        {FieldId{6}, AttributeValue{std::vector<std::byte>{std::byte{0x01}, std::byte{0xff}}}},
        {FieldId{7}, AttributeValue{ObjectId{99}}},
        {FieldId{8}, AttributeValue{BlobId{123}}},
    };

    auto encoded = encode_object(ObjectId{16}, TypeDefinitionId{7}, fields);
    suite.check(encoded.has_value(), "an object with every value type encodes");
    if (!encoded) {
        return;
    }
    auto decoded = decode_object(*encoded);
    suite.check(decoded.has_value(), "the encoded object decodes");
    if (!decoded) {
        return;
    }
    suite.check(decoded->id == ObjectId{16}, "object id survives the round-trip");
    suite.check(decoded->type == TypeDefinitionId{7}, "type id survives the round-trip");
    suite.check(decoded->fields == fields, "all field values survive the round-trip");
}

// Um objeto sem campos (só o cabeçalho) também faz round-trip.
void test_round_trip_empty(TestSuite& suite) {
    auto encoded = encode_object(ObjectId{20}, TypeDefinitionId{16}, {});
    suite.check(encoded.has_value(), "an object with no fields encodes");
    if (!encoded) {
        return;
    }
    auto decoded = decode_object(*encoded);
    suite.check(decoded.has_value() && decoded->fields.empty(),
                "an object with no fields decodes to no fields");
}

// Monta manualmente um payload com uma contagem de campos mentirosa.
void test_lying_field_count(TestSuite& suite) {
    std::vector<std::byte> record;
    auto push_u64 = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            record.push_back(std::byte{static_cast<std::uint8_t>(v & 0xffU)});
            v >>= 8U;
        }
    };
    push_u64(16);                       // object_id
    push_u64(7);                        // type_definition_id
    record.push_back(std::byte{1});     // versão do payload
    // field_count = 1000, mas o payload acaba logo em seguida.
    record.push_back(std::byte{0xe8});  // 1000 & 0xff
    record.push_back(std::byte{0x03});  // 1000 >> 8
    record.push_back(std::byte{0x00});  // um byte solto qualquer

    auto decoded = decode_object(record);
    suite.check_error(decoded, ErrorCode::too_many_columns,
                      "a lying field_count above the ADR-007 cap is rejected before allocating");
}

// Uma tag de tipo desconhecida é rejeitada.
void test_unknown_tag(TestSuite& suite) {
    std::vector<std::byte> record;
    auto push_u64 = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            record.push_back(std::byte{static_cast<std::uint8_t>(v & 0xffU)});
            v >>= 8U;
        }
    };
    push_u64(16);
    push_u64(7);
    record.push_back(std::byte{1});     // versão
    record.push_back(std::byte{0x01});  // field_count (u16) = 1
    record.push_back(std::byte{0x00});
    record.push_back(std::byte{0x01});  // field_id = 1
    record.push_back(std::byte{0x00});
    record.push_back(std::byte{99});    // tag inválida

    suite.check_error(decode_object(record), ErrorCode::invalid_encoding,
                      "an unknown attribute tag is rejected");
}

// Um comprimento de string maior que o buffer é rejeitado sem OOB.
void test_string_length_past_buffer(TestSuite& suite) {
    const FieldValues fields{{FieldId{1}, AttributeValue{"Ana"}}};
    auto encoded = encode_object(ObjectId{16}, TypeDefinitionId{7}, fields);
    if (!encoded) {
        suite.check(false, "fixture object encodes");
        return;
    }
    // Corrompe o comprimento da string (u32 logo após a tag) para um valor
    // gigante: header 8+8, versão 1, count 2, field_id 2, tag 1 => len em [22..26).
    auto record = *encoded;
    for (std::size_t i = 22; i < 26 && i < record.size(); ++i) {
        record[i] = std::byte{0xff};
    }
    suite.check_error(decode_object(record), ErrorCode::unexpected_end_of_input,
                      "a string length past the buffer is rejected");
}

// Bytes sobrando depois do último campo são rejeitados.
void test_trailing_data(TestSuite& suite) {
    auto encoded = encode_object(ObjectId{16}, TypeDefinitionId{7},
                                 FieldValues{{FieldId{1}, AttributeValue{std::int64_t{1}}}});
    if (!encoded) {
        suite.check(false, "fixture object encodes");
        return;
    }
    auto record = *encoded;
    record.push_back(std::byte{0x00});  // um byte a mais
    suite.check_error(decode_object(record), ErrorCode::trailing_data,
                      "trailing bytes after the last field are rejected");
}

// Um FieldId repetido no payload é rejeitado.
void test_duplicate_field(TestSuite& suite) {
    std::vector<std::byte> record;
    auto push_u64 = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            record.push_back(std::byte{static_cast<std::uint8_t>(v & 0xffU)});
            v >>= 8U;
        }
    };
    push_u64(16);
    push_u64(7);
    record.push_back(std::byte{1});     // versão
    record.push_back(std::byte{0x02});  // field_count (u16) = 2
    record.push_back(std::byte{0x00});
    // campo 1: field_id 1, boolean true
    record.push_back(std::byte{0x01});
    record.push_back(std::byte{0x00});
    record.push_back(std::byte{static_cast<std::uint8_t>(AttributeType::boolean)});
    record.push_back(std::byte{1});
    // campo 2: field_id 1 de novo (duplicado), boolean false
    record.push_back(std::byte{0x01});
    record.push_back(std::byte{0x00});
    record.push_back(std::byte{static_cast<std::uint8_t>(AttributeType::boolean)});
    record.push_back(std::byte{0});

    suite.check_error(decode_object(record), ErrorCode::invalid_encoding,
                      "a duplicate FieldId in the payload is rejected");
}

void test_decode_object_header(TestSuite& suite) {
    const FieldValues fields{{FieldId{1}, AttributeValue{std::int64_t{7}}}};
    auto encoded = encode_object(ObjectId{42}, TypeDefinitionId{99}, fields);
    suite.check(encoded.has_value(), "encode for header peek");
    if (!encoded) {
        return;
    }
    auto header = decode_object_header(*encoded);
    suite.check(header.has_value() && header->id.value == 42 && header->type.value == 99,
                "decode_object_header reads id and type without payload fields");
}

} // namespace

int main() {
    TestSuite suite;
    test_round_trip_all_types(suite);
    test_round_trip_empty(suite);
    test_lying_field_count(suite);
    test_unknown_tag(suite);
    test_string_length_past_buffer(suite);
    test_trailing_data(suite);
    test_duplicate_field(suite);
    test_decode_object_header(suite);
    return suite.finish();
}
