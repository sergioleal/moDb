#pragma once

// Importa ObjectId, a identidade que uma referência guarda.
#include "modb/object/ids.hpp"

namespace modb::object {

// Associação (◇): guarda o ObjectId do alvo e resolve para ele sob demanda.
// Remover o alvo é permitido; a resolução posterior falha de forma detectável
// (ADR-008). Persiste como tag `ref`.
template <typename T>
struct Ref {
    ObjectId target{};
    friend bool operator==(Ref, Ref) = default;
};

// Composição (◆): mesma representação de Ref, mas a remoção do pai remove o
// filho em cascata. A distinção associação/composição vive na
// `AttributeDefinition.is_owned`. Persiste como tag `ref`.
template <typename T>
struct OwnedRef {
    ObjectId target{};
    friend bool operator==(OwnedRef, OwnedRef) = default;
};

// Objeto embutido sem identidade própria: serializa no payload do pai (tag
// `embedded`). Exige um Binding do tipo aninhado no momento do bind.
template <typename T>
struct Embedded {
    T value{};
    friend bool operator==(const Embedded&, const Embedded&) = default;
};

// Traços para o binding reconhecer os três em tempo de compilação.
template <typename>
inline constexpr bool is_ref_v = false;
template <typename T>
inline constexpr bool is_ref_v<Ref<T>> = true;

template <typename>
inline constexpr bool is_owned_ref_v = false;
template <typename T>
inline constexpr bool is_owned_ref_v<OwnedRef<T>> = true;

template <typename>
inline constexpr bool is_embedded_v = false;
template <typename T>
inline constexpr bool is_embedded_v<Embedded<T>> = true;

} // namespace modb::object
