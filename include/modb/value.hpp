#pragma once

// Importa a enumeração dos tipos suportados.
#include "modb/data_type.hpp"

// Disponibiliza conceitos usados para restringir o construtor de inteiros.
#include <concepts>
// Disponibiliza inteiros com largura conhecida.
#include <cstdint>
// Disponibiliza std::optional para um tipo que pode não existir em NULL.
#include <optional>
// Disponibiliza o armazenamento de textos.
#include <string>
// Disponibiliza uma visualização leve de texto.
#include <string_view>
// Disponibiliza std::variant para guardar tipos diferentes com segurança.
#include <variant>

namespace modb {

// Tipo marcador usado para representar o valor SQL NULL.
struct Null {
    // Todos os objetos Null representam o mesmo valor.
    friend bool operator==(Null, Null) = default;
};

// Armazena um único valor relacional com seu tipo preservado.
class Value {
public:
    // Define todas as representações que um Value pode conter.
    using Storage = std::variant<Null, bool, std::int64_t, double, std::string>;

    // Cria NULL por padrão.
    Value() noexcept;
    // Cria explicitamente um valor NULL.
    Value(Null) noexcept;
    // Cria um BOOLEAN.
    Value(bool value) noexcept;

    // Aceita qualquer inteiro com sinal, exceto bool.
    template <std::signed_integral Integer>
        requires(!std::same_as<Integer, bool>)
    // Converte o inteiro recebido para a representação INTEGER de 64 bits.
    Value(Integer value) noexcept : storage_{static_cast<std::int64_t>(value)} {}

    // Cria um REAL.
    Value(double value) noexcept;
    // Cria um TEXT assumindo a propriedade da string recebida.
    Value(std::string value);
    // Cria um TEXT copiando o conteúdo da visualização.
    Value(std::string_view value);
    // Facilita a criação de TEXT a partir de um literal como "Ana".
    Value(const char* value);

    // Impede que um ponteiro qualquer vire BOOLEAN silenciosamente via T*->bool.
    // A restrição preserva a construção a partir de char*/const char* (que usam o
    // construtor de texto acima); qualquer outro ponteiro passa a ser erro de
    // compilação em vez de um booleano acidental.
    template <typename Pointer>
        requires(!std::same_as<Pointer, char>)
    Value(Pointer*) = delete;

    // Informa se este valor representa NULL.
    [[nodiscard]] bool is_null() const noexcept;
    // Retorna o tipo do valor ou vazio quando ele é NULL.
    [[nodiscard]] std::optional<DataType> data_type() const noexcept;
    // Dá acesso somente leitura ao variant interno.
    [[nodiscard]] const Storage& storage() const noexcept;

    // Compara o tipo e o conteúdo de dois valores.
    friend bool operator==(const Value&, const Value&) = default;

private:
    // Guarda efetivamente o conteúdo do valor.
    Storage storage_;
};

} // namespace modb
