#pragma once

// Importa Result e os códigos de erro de codificação.
#include "modb/error.hpp"
// Importa AttributeValue, usado ao codificar/decodificar cada campo.
#include "modb/object/attribute_value.hpp"
// Importa ObjectId/TypeDefinitionId e FieldValues.
#include "modb/object/ids.hpp"
#include "modb/object/type_definition.hpp"
// Importa BinaryWriter/BinaryReader usados pelo codec de valor único.
#include "modb/storage/binary.hpp"

// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza a visão de bytes decodificada.
#include <span>
// Disponibiliza o buffer devolvido pela codificação.
#include <vector>

namespace modb::object {

// Versão do formato do payload de objeto (ADR-003 / Fase 2 do protocolo).
inline constexpr std::uint8_t object_payload_version = 1;

// Um objeto decodificado a partir de um registro do TableHeap. O codec é
// genérico: não conhece classes C++ nem exige a TypeDefinition para decodificar
// (o payload é autodescritivo por tags). A validação semântica contra o tipo é
// responsabilidade de quem chama (ObjectStore).
struct DecodedObject {
    ObjectId id;
    TypeDefinitionId type;
    FieldValues fields;

    friend bool operator==(const DecodedObject&, const DecodedObject&) = default;
};

// Codifica um objeto completo no formato de registro do TableHeap:
//   | object_id u64 | type_definition_id u64 | payload |
// com o payload sendo | versão u8 | field_count u16 | (field_id u16, tag u8,
// valor)* |. Recebe o TypeDefinitionId em vez da TypeDefinition inteira porque
// nada além do id é necessário para gravar — a compatibilidade dos valores com
// o tipo é checada por validate_object antes desta chamada.
[[nodiscard]] Result<std::vector<std::byte>> encode_object(ObjectId id, TypeDefinitionId type,
                                                           const FieldValues& fields);

// Decodifica um registro. Toda leitura valida limites antes de acessar bytes
// (o registro vem de um arquivo não confiável): contagem mentirosa, tag
// desconhecida, comprimento além do buffer, campo duplicado e bytes sobrando
// viram erro, nunca acesso fora de faixa nem alocação gigante.
[[nodiscard]] Result<DecodedObject> decode_object(std::span<const std::byte> record);

// Payload lógico sem identidade (Fase 8C / ObjectEnvelope):
//   | versão u8 | field_count u16 | (field_id u16, tag u8, valor)* |
// A identidade viaja no envelope da rede; o registro do heap usa encode_object.
[[nodiscard]] Result<std::vector<std::byte>> encode_object_payload(const FieldValues& fields);
[[nodiscard]] Result<FieldValues> decode_object_payload(std::span<const std::byte> payload);

// Escreve um único valor (tag u8 + conteúdo no encoding do ADR-003). Exposto
// porque o catálogo (CatalogStore) reaproveita o mesmo encoding ao serializar
// defaults de atributo dentro do seu sub-formato.
[[nodiscard]] Result<void> encode_value(storage::BinaryWriter& writer, const AttributeValue& value);
// Lê um único valor gravado por encode_value, validando limites.
[[nodiscard]] Result<AttributeValue> decode_value(storage::BinaryReader& reader);

} // namespace modb::object
