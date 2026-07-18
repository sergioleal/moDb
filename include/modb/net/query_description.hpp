#pragma once

// Descrição serializável de uma consulta remota (Fase 8A). É o contrato que
// a mensagem `Query` carrega pela rede — declarativa e restrita, sem
// templates nem std::function. A execução concreta (planner/pipeline) fica
// para a Fase 8C.

#include "modb/error.hpp"
#include "modb/object/attribute_value.hpp"
#include "modb/object/ids.hpp"
#include "modb/storage/binary.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace modb::net {

// Predicado de igualdade serializável (campo + valor canônico ADR-003).
struct EqualityFilter {
    object::FieldId field{};
    object::AttributeValue value{};

    friend bool operator==(const EqualityFilter&, const EqualityFilter&) = default;
};

// Consulta remota mínima: tipo alvo, limite opcional, igualdade opcional e
// projeção opcional (lista vazia = todos os campos). Extensões (faixa,
// ordenação, agregação) entram em entregas posteriores sem quebrar o
// versionamento do envelope da mensagem.
struct QueryDescription {
    object::TypeDefinitionId type{};
    // 0 = sem limite.
    std::uint64_t limit{0};
    std::optional<EqualityFilter> equals{};
    std::vector<object::FieldId> project{};

    friend bool operator==(const QueryDescription&, const QueryDescription&) = default;
};

[[nodiscard]] Result<void> encode_query_description(storage::BinaryWriter& writer,
                                                    const QueryDescription& description);
[[nodiscard]] Result<QueryDescription> decode_query_description(storage::BinaryReader& reader);

} // namespace modb::net
