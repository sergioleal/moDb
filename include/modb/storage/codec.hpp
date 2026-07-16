#pragma once

// Importa Result e os erros de decodificação.
#include "modb/error.hpp"
// Importa Row, Schema e Value convertidos pelos codecs.
#include "modb/row.hpp"
#include "modb/schema.hpp"
#include "modb/value.hpp"

// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza visões dos bytes recebidos.
#include <span>
// Disponibiliza o vetor retornado pela codificação.
#include <vector>

namespace modb::storage {

// Converte um Value completo em bytes.
[[nodiscard]] Result<std::vector<std::byte>> encode_value(const Value& value);
// Reconstrói um Value e rejeita bytes ausentes ou adicionais.
[[nodiscard]] Result<Value> decode_value(std::span<const std::byte> bytes);

// Converte uma Row completa em bytes.
[[nodiscard]] Result<std::vector<std::byte>> encode_row(const Row& row);
// Reconstrói uma Row e rejeita bytes ausentes ou adicionais.
[[nodiscard]] Result<Row> decode_row(std::span<const std::byte> bytes);

// Converte um Schema completo em bytes.
[[nodiscard]] Result<std::vector<std::byte>> encode_schema(const Schema& schema);
// Reconstrói e valida um Schema a partir dos bytes.
[[nodiscard]] Result<Schema> decode_schema(std::span<const std::byte> bytes);

} // namespace modb::storage

