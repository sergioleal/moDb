// Importa a declaração de AttributeValue.
#include "modb/object/attribute_value.hpp"
// Importa o helper de visitação exaustiva de variant.
#include "modb/detail/overloaded.hpp"

// Disponibiliza std::move para transferir strings e vetores.
#include <utility>
// Disponibiliza std::get/std::holds_alternative e std::visit.
#include <variant>

namespace modb::object {

// Converte a tag para um nome estável usado em diagnósticos.
std::string_view attribute_type_name(AttributeType type) noexcept {
    // Escolhe o texto correspondente à tag recebida.
    switch (type) {
    case AttributeType::null:
        return "NULL";
    case AttributeType::boolean:
        return "BOOLEAN";
    case AttributeType::int64:
        return "INT64";
    case AttributeType::float64:
        return "FLOAT64";
    case AttributeType::string:
        return "STRING";
    case AttributeType::bytes:
        return "BYTES";
    case AttributeType::ref:
        return "REF";
    case AttributeType::blob:
        return "BLOB";
    case AttributeType::embedded:
        return "EMBEDDED";
    }
    // Protege a função caso um valor inválido seja convertido para o enum.
    return "UNKNOWN";
}

// Um AttributeValue criado sem argumento representa o estado ausente.
AttributeValue::AttributeValue() noexcept : storage_{AttributeNull{}} {}

// Armazena explicitamente o marcador de ausência.
AttributeValue::AttributeValue(AttributeNull) noexcept : storage_{AttributeNull{}} {}

// Armazena um valor booleano no variant.
AttributeValue::AttributeValue(bool value) noexcept : storage_{value} {}

// Armazena um número de ponto flutuante no variant.
AttributeValue::AttributeValue(double value) noexcept : storage_{value} {}

// Move uma string existente para o variant, evitando outra cópia.
AttributeValue::AttributeValue(std::string value) : storage_{std::move(value)} {}

// Copia o texto apontado pela string_view para uma string própria.
AttributeValue::AttributeValue(std::string_view value) : storage_{std::string{value}} {}

// Copia uma string terminada em zero, como um literal, para o variant.
AttributeValue::AttributeValue(const char* value) : storage_{std::string{value}} {}

// Move um vetor de bytes existente para o variant.
AttributeValue::AttributeValue(std::vector<std::byte> value) : storage_{std::move(value)} {}

// Armazena uma referência a outro objeto.
AttributeValue::AttributeValue(ObjectId value) noexcept : storage_{value} {}

// Armazena uma referência a um blob.
AttributeValue::AttributeValue(BlobId value) noexcept : storage_{value} {}

// Move o sub-objeto codificado de um valor embedded para o variant.
AttributeValue::AttributeValue(EmbeddedValue value) : storage_{std::move(value)} {}

// Verifica se o tipo ativo do variant é o marcador de ausência.
bool AttributeValue::is_null() const noexcept {
    return std::holds_alternative<AttributeNull>(storage_);
}

// Descobre qual AttributeType corresponde ao conteúdo ativo.
AttributeType AttributeValue::type() const noexcept {
    // std::visit garante em compilação que toda alternativa do variant é
    // tratada: uma nova alternativa sem caso aqui vira erro de compilação,
    // não bug em runtime. Ao contrário do Value relacional, null é uma tag
    // de tipo válida, então este retorno nunca é opcional.
    return std::visit(
        detail::Overloaded{
            [](AttributeNull) { return AttributeType::null; },
            [](bool) { return AttributeType::boolean; },
            [](std::int64_t) { return AttributeType::int64; },
            [](double) { return AttributeType::float64; },
            [](const std::string&) { return AttributeType::string; },
            [](const std::vector<std::byte>&) { return AttributeType::bytes; },
            [](ObjectId) { return AttributeType::ref; },
            [](BlobId) { return AttributeType::blob; },
            [](const EmbeddedValue&) { return AttributeType::embedded; },
        },
        storage_);
}

// Retorna o variant somente para leitura e sem copiá-lo.
const AttributeValue::Storage& AttributeValue::storage() const noexcept { return storage_; }

namespace {

// Monta o erro comum a todos os accessors tipados quando a tag não bate.
Error type_mismatch_for(AttributeType actual, std::string_view expected) {
    return Error{
        ErrorCode::type_mismatch,
        "attribute value is " + std::string{attribute_type_name(actual)} + ", not " +
            std::string{expected},
    };
}

} // namespace

Result<bool> AttributeValue::as_bool() const {
    if (const auto* value = std::get_if<bool>(&storage_)) {
        return *value;
    }
    return std::unexpected(type_mismatch_for(type(), "BOOLEAN"));
}

Result<std::int64_t> AttributeValue::as_int64() const {
    if (const auto* value = std::get_if<std::int64_t>(&storage_)) {
        return *value;
    }
    return std::unexpected(type_mismatch_for(type(), "INT64"));
}

Result<double> AttributeValue::as_float64() const {
    if (const auto* value = std::get_if<double>(&storage_)) {
        return *value;
    }
    return std::unexpected(type_mismatch_for(type(), "FLOAT64"));
}

Result<std::string_view> AttributeValue::as_string() const {
    if (const auto* value = std::get_if<std::string>(&storage_)) {
        return std::string_view{*value};
    }
    return std::unexpected(type_mismatch_for(type(), "STRING"));
}

Result<std::span<const std::byte>> AttributeValue::as_bytes() const {
    if (const auto* value = std::get_if<std::vector<std::byte>>(&storage_)) {
        return std::span<const std::byte>{*value};
    }
    return std::unexpected(type_mismatch_for(type(), "BYTES"));
}

Result<ObjectId> AttributeValue::as_ref() const {
    if (const auto* value = std::get_if<ObjectId>(&storage_)) {
        return *value;
    }
    return std::unexpected(type_mismatch_for(type(), "REF"));
}

Result<BlobId> AttributeValue::as_blob() const {
    if (const auto* value = std::get_if<BlobId>(&storage_)) {
        return *value;
    }
    return std::unexpected(type_mismatch_for(type(), "BLOB"));
}

Result<std::span<const std::byte>> AttributeValue::as_embedded() const {
    if (const auto* value = std::get_if<EmbeddedValue>(&storage_)) {
        return std::span<const std::byte>{value->payload};
    }
    return std::unexpected(type_mismatch_for(type(), "EMBEDDED"));
}

} // namespace modb::object
