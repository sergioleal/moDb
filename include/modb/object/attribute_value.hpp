#pragma once

// Importa Result e os códigos de erro usados pelos accessors tipados.
#include "modb/error.hpp"
// Importa ObjectId e BlobId, guardados como valor de atributo (ref/blob).
#include "modb/object/ids.hpp"

// Disponibiliza conceitos usados para restringir o construtor de inteiros.
#include <concepts>
// Disponibiliza std::byte, usado pelo tipo BYTES.
#include <cstddef>
// Disponibiliza inteiros com largura conhecida.
#include <cstdint>
// Disponibiliza a visão sem cópia devolvida por as_bytes.
#include <span>
// Disponibiliza o armazenamento de textos e binários.
#include <string>
#include <string_view>
// Disponibiliza std::variant para guardar tipos diferentes com segurança.
#include <variant>
// Disponibiliza o contêiner por trás do tipo BYTES.
#include <vector>

namespace modb::object {

// Tags de tipo de atributo e seu encoding no payload (ADR-003). A largura e
// os valores numéricos são parte do formato binário: nunca renumerar.
enum class AttributeType : std::uint8_t {
    null = 0,
    boolean = 1,
    int64 = 2,
    float64 = 3,
    string = 4,
    bytes = 5,
    ref = 6,
    blob = 7,
    embedded = 8,
};

// Retorna um nome estável para diagnósticos e mensagens de erro.
[[nodiscard]] std::string_view attribute_type_name(AttributeType type) noexcept;

// Marcador para o valor ausente de um atributo. Ao contrário do Value
// relacional, NULL é uma tag de tipo válida (AttributeType::null) e não a
// ausência de tipo — por isso AttributeValue::type() nunca é opcional.
struct AttributeNull {
    friend bool operator==(AttributeNull, AttributeNull) = default;
};

// Objeto embutido (Embedded<T>, Fase 4): guarda o sub-objeto já codificado
// (`field_count u16 | (field_id u16, valor)*`), opaco no nível do codec. A
// estrutura aninhada é reconstruída pelo Binding do tipo embutido, não aqui —
// isso evita um variant recursivo em AttributeValue.
struct EmbeddedValue {
    std::vector<std::byte> payload;
    friend bool operator==(const EmbeddedValue&, const EmbeddedValue&) = default;
};

// Armazena um único valor de atributo com seu tipo preservado.
class AttributeValue {
public:
    // Define todas as representações que um AttributeValue pode conter.
    using Storage = std::variant<AttributeNull, bool, std::int64_t, double, std::string,
                                 std::vector<std::byte>, ObjectId, BlobId, EmbeddedValue>;

    // Cria o valor ausente (tag null) por padrão.
    AttributeValue() noexcept;
    // Cria explicitamente o valor ausente.
    AttributeValue(AttributeNull) noexcept;
    // Cria um BOOLEAN.
    AttributeValue(bool value) noexcept;

    // Aceita qualquer inteiro com sinal, exceto bool.
    template <std::signed_integral Integer>
        requires(!std::same_as<Integer, bool>)
    // Converte o inteiro recebido para a representação INT64.
    AttributeValue(Integer value) noexcept : storage_{static_cast<std::int64_t>(value)} {}

    // Cria um FLOAT64.
    AttributeValue(double value) noexcept;
    // Cria um STRING assumindo a propriedade da string recebida.
    AttributeValue(std::string value);
    // Cria um STRING copiando o conteúdo da visualização.
    AttributeValue(std::string_view value);
    // Facilita a criação de STRING a partir de um literal como "Ana".
    AttributeValue(const char* value);
    // Cria um BYTES assumindo a propriedade do vetor recebido.
    AttributeValue(std::vector<std::byte> value);
    // Cria uma referência (tag ref) para outro objeto.
    AttributeValue(ObjectId value) noexcept;
    // Cria uma referência (tag blob) para dados grandes na BlobStore.
    AttributeValue(BlobId value) noexcept;
    // Cria um objeto embutido (tag embedded) a partir do sub-objeto codificado.
    AttributeValue(EmbeddedValue value);

    // Impede que um ponteiro qualquer vire BOOLEAN silenciosamente via T*->bool,
    // preservando a construção a partir de char*/const char* (texto).
    template <typename Pointer>
        requires(!std::same_as<Pointer, char>)
    AttributeValue(Pointer*) = delete;

    // Informa se este valor representa o estado ausente (tag null).
    [[nodiscard]] bool is_null() const noexcept;
    // Retorna a tag de tipo do valor; sempre definida (null incluso).
    [[nodiscard]] AttributeType type() const noexcept;
    // Dá acesso somente leitura ao variant interno.
    [[nodiscard]] const Storage& storage() const noexcept;

    // Accessors tipados: retornam Result em vez de arriscar bad_variant_access
    // quando o valor guardado não é do tipo pedido.
    [[nodiscard]] Result<bool> as_bool() const;
    [[nodiscard]] Result<std::int64_t> as_int64() const;
    [[nodiscard]] Result<double> as_float64() const;
    [[nodiscard]] Result<std::string_view> as_string() const;
    [[nodiscard]] Result<std::span<const std::byte>> as_bytes() const;
    [[nodiscard]] Result<ObjectId> as_ref() const;
    [[nodiscard]] Result<BlobId> as_blob() const;
    // Devolve o sub-objeto codificado de um valor embedded, sem copiá-lo.
    [[nodiscard]] Result<std::span<const std::byte>> as_embedded() const;

    // Compara o tipo e o conteúdo de dois valores.
    friend bool operator==(const AttributeValue&, const AttributeValue&) = default;

private:
    // Guarda efetivamente o conteúdo do valor.
    Storage storage_;
};

} // namespace modb::object
