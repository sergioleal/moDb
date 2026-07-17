#pragma once

// Importa Result e os códigos de erro.
#include "modb/error.hpp"
// Importa AttributeValue/AttributeType.
#include "modb/object/attribute_value.hpp"
// Importa FieldId.
#include "modb/object/ids.hpp"
// Importa Ref/OwnedRef/Embedded e seus traços.
#include "modb/object/ref.hpp"
// Importa TypeDefinition e FieldValues (a forma genérica de um objeto).
#include "modb/object/type_definition.hpp"

// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza os conceitos usados no mapeamento de tipos.
#include <concepts>
// Disponibiliza os acessores type-erased de cada campo.
#include <functional>
// Disponibiliza limites para conversões numéricas seguras.
#include <limits>
// Disponibiliza o Binding aninhado compartilhado pelos binders embedded.
#include <memory>
// Disponibiliza o default opcional de cada campo.
#include <optional>
// Disponibiliza a visão sem cópia dos campos.
#include <span>
// Disponibiliza o nome do tipo e dos atributos.
#include <string>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza o contêiner de FieldBinder.
#include <vector>

namespace modb::object {

// Marcador dependente para mensagens de static_assert legíveis.
template <typename>
inline constexpr bool binding_unsupported_type = false;

// Mapeia um tipo de membro C++ para a tag de atributo persistente (ADR-003).
// ObjectId cobre a referência crua; Ref<T>/OwnedRef<T> mapeiam para a mesma tag
// `ref` (a distinção associação/composição vive em is_owned). Embedded<T> usa o
// método dedicado BindingBuilder::embedded, não este mapeamento.
template <typename Member>
consteval AttributeType attribute_type_of() {
    if constexpr (std::same_as<Member, bool>) {
        return AttributeType::boolean;
    } else if constexpr (std::same_as<Member, std::string>) {
        return AttributeType::string;
    } else if constexpr (std::same_as<Member, std::vector<std::byte>>) {
        return AttributeType::bytes;
    } else if constexpr (std::same_as<Member, ObjectId>) {
        return AttributeType::ref;
    } else if constexpr (std::same_as<Member, BlobId>) {
        return AttributeType::blob;
    } else if constexpr (is_ref_v<Member> || is_owned_ref_v<Member>) {
        return AttributeType::ref;
    } else if constexpr (std::signed_integral<Member>) {
        return AttributeType::int64;
    } else if constexpr (std::same_as<Member, double>) {
        return AttributeType::float64;
    } else {
        static_assert(binding_unsupported_type<Member>,
                      "type has no AttributeType mapping (use embedded() for Embedded<T>)");
    }
}

// Converte um valor de membro C++ em AttributeValue para persistir.
template <typename Member>
AttributeValue to_attribute_value(const Member& member) {
    if constexpr (is_ref_v<Member> || is_owned_ref_v<Member>) {
        return AttributeValue{member.target};
    } else if constexpr (std::same_as<Member, bool>) {
        return AttributeValue{member};
    } else if constexpr (std::signed_integral<Member>) {
        return AttributeValue{static_cast<std::int64_t>(member)};
    } else if constexpr (std::same_as<Member, double>) {
        return AttributeValue{member};
    } else {
        return AttributeValue{member};
    }
}

// Converte um AttributeValue de volta em um valor de membro C++.
template <typename Member>
Result<Member> from_attribute_value(const AttributeValue& value) {
    if constexpr (is_ref_v<Member> || is_owned_ref_v<Member>) {
        auto target = value.as_ref();
        if (!target) {
            return std::unexpected(target.error());
        }
        return Member{*target};
    } else if constexpr (std::same_as<Member, bool>) {
        return value.as_bool();
    } else if constexpr (std::signed_integral<Member>) {
        auto raw = value.as_int64();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        if (*raw < static_cast<std::int64_t>(std::numeric_limits<Member>::lowest()) ||
            *raw > static_cast<std::int64_t>(std::numeric_limits<Member>::max())) {
            return std::unexpected(
                Error{ErrorCode::incompatible_projection, "integer member conversion overflow"});
        }
        return static_cast<Member>(*raw);
    } else if constexpr (std::same_as<Member, double>) {
        auto raw = value.as_float64();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        return static_cast<Member>(*raw);
    } else if constexpr (std::same_as<Member, std::string>) {
        auto raw = value.as_string();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        return std::string{*raw};
    } else if constexpr (std::same_as<Member, ObjectId>) {
        return value.as_ref();
    } else if constexpr (std::same_as<Member, BlobId>) {
        return value.as_blob();
    } else {
        auto raw = value.as_bytes();
        if (!raw) {
            return std::unexpected(raw.error());
        }
        return std::vector<std::byte>{raw->begin(), raw->end()};
    }
}

// Liga um FieldId a um membro C++ por meio de acessores type-erased (o objeto é
// visto como void*; cada lambda conhece o tipo concreto por dentro).
struct FieldBinder {
    FieldId id;
    std::string name;
    AttributeType type{AttributeType::null};
    // Referência de composição (OwnedRef<T>): remover o pai remove o filho.
    bool is_owned{false};
    // Objeto embutido sem identidade própria (Embedded<T>).
    bool is_embedded{false};
    // Valor usado ao projetar uma definição antiga que não possui este campo.
    std::optional<AttributeValue> default_value;
    // Escreve um AttributeValue no membro do objeto (erro se o tipo não bate).
    std::function<Result<void>(void*, const AttributeValue&)> store;
    // Lê o membro do objeto como AttributeValue. Falível porque um campo
    // embedded precisa codificar o sub-objeto, o que pode estourar limites.
    std::function<Result<AttributeValue>(const void*)> load;
};

// Codifica/decodifica o sub-formato de um objeto embutido
// (`field_count u16 | (field_id u16, valor)*`). Declarados aqui e definidos no
// .cpp porque os binders embedded (template) os chamam do header.
[[nodiscard]] Result<std::vector<std::byte>> encode_embedded(const FieldValues& fields);
[[nodiscard]] Result<FieldValues> decode_embedded(std::span<const std::byte> payload);

// Declara BindingBuilder para conceder acesso ao construtor privado de Binding.
template <typename T>
class BindingBuilder;

// Liga um tipo C++ (via seus membros) ao formato persistente. Não é template: a
// dependência de T fica encapsulada nos FieldBinders (acessores por void*), o
// que permite ao ProjectionPlan e ao codec consumirem o Binding sem conhecer T.
class Binding {
public:
    [[nodiscard]] const std::string& type_name() const noexcept { return type_name_; }
    [[nodiscard]] std::span<const FieldBinder> fields() const noexcept { return fields_; }

    // Gera a TypeDefinition canônica deste binding (ainda sem id atribuído),
    // usada para registrar/comparar o tipo no catálogo. Campos de binding são
    // sempre presentes, portanto não-nulos e sem default.
    [[nodiscard]] Result<TypeDefinition> to_type_definition() const;

    // Extrai os valores de um objeto C++ para a forma genérica persistível.
    // Falível: campos embedded codificam o sub-objeto ao serem lidos.
    [[nodiscard]] Result<FieldValues> to_field_values(const void* object) const;

    // Preenche um objeto C++ a partir dos campos decodificados, casando por
    // FieldId. Nesta fase a projeção é identidade (mesmo binding do que gravou);
    // conversão/valor-default entram com o ProjectionPlan.
    [[nodiscard]] Result<void> materialize(const FieldValues& fields, void* destination) const;

private:
    template <typename U>
    friend class BindingBuilder;

    Binding(std::string type_name, std::vector<FieldBinder> fields)
        : type_name_{std::move(type_name)}, fields_{std::move(fields)} {}

    std::string type_name_;
    std::vector<FieldBinder> fields_;
};

// Constrói um Binding para o tipo T de forma fluente e validada.
template <typename T>
class BindingBuilder {
public:
    explicit BindingBuilder(std::string type_name) : type_name_{std::move(type_name)} {}

    // Liga o FieldId Id ao membro dado; o tipo do atributo vem do tipo do membro.
    template <std::uint16_t Id, typename Member>
    BindingBuilder& field(std::string name, Member T::* member) {
        return add_field<Id>(std::move(name), member, std::nullopt);
    }

    // Define também o default usado na evolução de schema.
    template <std::uint16_t Id, typename Member, typename Default>
        requires std::constructible_from<Member, Default>
    BindingBuilder& field(std::string name, Member T::* member, Default&& default_value) {
        Member converted{std::forward<Default>(default_value)};
        return add_field<Id>(std::move(name), member,
                             std::optional<AttributeValue>{to_attribute_value(converted)});
    }

    // Liga um membro Embedded<U> ao Binding do tipo aninhado. O sub-objeto é
    // serializado no payload do pai (tag embedded), sem identidade própria.
    template <std::uint16_t Id, typename U>
    BindingBuilder& embedded(std::string name, Embedded<U> T::* member, Binding child) {
        auto shared = std::make_shared<Binding>(std::move(child));
        FieldBinder binder;
        binder.id = FieldId{Id};
        binder.name = std::move(name);
        binder.type = AttributeType::embedded;
        binder.is_embedded = true;
        binder.store = [member, shared](void* object, const AttributeValue& value) -> Result<void> {
            auto payload = value.as_embedded();
            if (!payload) {
                return std::unexpected(payload.error());
            }
            auto fields = decode_embedded(*payload);
            if (!fields) {
                return std::unexpected(fields.error());
            }
            return shared->materialize(*fields, &(static_cast<T*>(object)->*member).value);
        };
        binder.load = [member, shared](const void* object) -> Result<AttributeValue> {
            auto fields = shared->to_field_values(&(static_cast<const T*>(object)->*member).value);
            if (!fields) {
                return std::unexpected(fields.error());
            }
            auto payload = encode_embedded(*fields);
            if (!payload) {
                return std::unexpected(payload.error());
            }
            return AttributeValue{EmbeddedValue{std::move(*payload)}};
        };
        fields_.push_back(std::move(binder));
        return *this;
    }

    // Valida ids únicos, nomes únicos e ao menos um campo, e devolve o Binding.
    [[nodiscard]] Result<Binding> build();

private:
    template <std::uint16_t Id, typename Member>
    BindingBuilder& add_field(std::string name, Member T::* member,
                              std::optional<AttributeValue> default_value) {
        FieldBinder binder;
        binder.id = FieldId{Id};
        binder.name = std::move(name);
        binder.type = attribute_type_of<Member>();
        binder.is_owned = is_owned_ref_v<Member>;
        binder.default_value = std::move(default_value);
        binder.store = [member](void* object, const AttributeValue& value) -> Result<void> {
            auto converted = from_attribute_value<Member>(value);
            if (!converted) {
                return std::unexpected(converted.error());
            }
            static_cast<T*>(object)->*member = std::move(*converted);
            return {};
        };
        binder.load = [member](const void* object) -> Result<AttributeValue> {
            return to_attribute_value<Member>(static_cast<const T*>(object)->*member);
        };
        fields_.push_back(std::move(binder));
        return *this;
    }
    std::string type_name_;
    std::vector<FieldBinder> fields_;
};

} // namespace modb::object

// A implementação de build() usa validações que dependem de utilidades do .cpp.
#include "modb/object/binding_builder.inl"
