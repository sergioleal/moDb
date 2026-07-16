// Importa a declaração de Value.
#include "modb/value.hpp"
// Importa o helper de visitação exaustiva de variant.
#include "modb/detail/overloaded.hpp"

// Disponibiliza std::move para transferir strings.
#include <utility>
// Disponibiliza std::visit para percorrer o variant.
#include <variant>

namespace modb {

// Um Value criado sem argumento representa NULL.
Value::Value() noexcept : storage_{Null{}} {}

// Armazena explicitamente o marcador NULL.
Value::Value(Null) noexcept : storage_{Null{}} {}

// Armazena um valor booleano no variant.
Value::Value(bool value) noexcept : storage_{value} {}

// Armazena um número real no variant.
Value::Value(double value) noexcept : storage_{value} {}

// Move uma string existente para o variant, evitando outra cópia.
Value::Value(std::string value) : storage_{std::move(value)} {}

// Copia o texto apontado pela string_view para uma string própria.
Value::Value(std::string_view value) : storage_{std::string{value}} {}

// Copia uma string terminada em zero, como um literal, para o variant.
Value::Value(const char* value) : storage_{std::string{value}} {}

// Verifica se o tipo ativo do variant é Null.
bool Value::is_null() const noexcept { return std::holds_alternative<Null>(storage_); }

// Descobre qual DataType corresponde ao conteúdo ativo.
std::optional<DataType> Value::data_type() const noexcept {
    // std::visit garante em compilação que toda alternativa do variant é tratada:
    // uma nova alternativa sem caso aqui vira erro de compilação, não bug em runtime.
    return std::visit(
        detail::Overloaded{
            // NULL não possui um DataType de coluna próprio.
            [](Null) -> std::optional<DataType> { return std::nullopt; },
            [](bool) -> std::optional<DataType> { return DataType::boolean; },
            [](std::int64_t) -> std::optional<DataType> { return DataType::integer; },
            [](double) -> std::optional<DataType> { return DataType::real; },
            [](const std::string&) -> std::optional<DataType> { return DataType::text; },
        },
        storage_);
}

// Retorna o variant somente para leitura e sem copiá-lo.
const Value::Storage& Value::storage() const noexcept { return storage_; }

} // namespace modb
