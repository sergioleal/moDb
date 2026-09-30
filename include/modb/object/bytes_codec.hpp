#pragma once

// Ponto de extensão do binding (PLANO_PROPOSTAS_REGISTRY R14): um tipo de membro
// que não é um dos tipos de atributo nativos, guardado num campo `bytes`.
//
// Especialize `bytes_codec<M>` com
//
//     static std::vector<std::byte> encode(const M&);
//     static Result<M> decode(std::span<const std::byte>);
//
// e o membro entra no BindingBuilder como qualquer outro (`field<N>(nome, &T::m)`).
// No arquivo, o campo é `bytes`: não muda o formato. O conteúdo não é indexável
// nem visível às consultas; trocar o codec de um campo já gravado é migração.
//
// `ops::Value` já tem o seu (modb/ops/value.hpp): um campo com uma lista de tags,
// um mapa ou um JSON livre fica no próprio objeto, sem texto para reinterpretar.

#include "modb/error.hpp"

#include <concepts>
#include <cstddef>
#include <span>
#include <vector>

namespace modb::object {

template <typename Member>
struct bytes_codec;

template <typename Member>
concept has_bytes_codec = requires(const Member& member, std::span<const std::byte> bytes) {
    { bytes_codec<Member>::encode(member) } -> std::same_as<std::vector<std::byte>>;
    { bytes_codec<Member>::decode(bytes) } -> std::same_as<Result<Member>>;
};

} // namespace modb::object
