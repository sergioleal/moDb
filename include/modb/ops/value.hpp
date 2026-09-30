#pragma once

// Valores autodescritos para os argumentos e resultados das stored procedures
// (PLANO_SERVIDOR_PROCS S2, ADR-025).
//
// Um `Value` é nulo, booleano, inteiro (int64), real (double), texto (UTF-8),
// id de objeto, lista ou mapa (chave texto). Viaja no OpCall/OpResult numa
// codificação binária versionada (`encode`/`decode`) e converte de e para JSON
// (`to_json`/`from_json`), para que um gateway web ou o CLI chamem procs sem
// conhecer TypeDefinitionId nem FieldId.
//
// Codificação (versão 1): `versão u8 | valor`, com
//   valor = tag u8 | conteúdo
//   null 0 | bool 1 u8 | integer 2 i64 | real 3 f64 (bits) | text 4 u32+bytes |
//   id 5 u64 | list 6 u32 + valores | map 7 u32 + (u32+chave, valor)...
// inteiros little-endian, como o resto do formato do moDb (ADR-003).

#include "modb/error.hpp"
#include "modb/object/bytes_codec.hpp"
#include "modb/object/ids.hpp"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace modb::ops {

class Value;
using ValueList = std::vector<Value>;
using ValueMap = std::map<std::string, Value, std::less<>>;

class Value {
public:
    enum class Kind : std::uint8_t { null = 0, boolean = 1, integer = 2, real = 3, text = 4, id = 5, list = 6, map = 7 };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : data_{v} {}
    Value(int v) : data_{std::int64_t{v}} {}
    Value(std::int64_t v) : data_{v} {}
    Value(double v) : data_{v} {}
    Value(std::string v) : data_{std::move(v)} {}
    Value(std::string_view v) : data_{std::string{v}} {}
    Value(const char* v) : data_{std::string{v}} {}
    Value(object::ObjectId v) : data_{v} {}
    Value(ValueList v) : data_{std::move(v)} {}
    Value(ValueMap v) : data_{std::move(v)} {}
    // Mapa literal: Value::object({{"id", h.id()}, {"titulo", "Dom Casmurro"}}).
    static Value object(std::initializer_list<std::pair<const std::string, Value>> items) { return ValueMap{items}; }

    [[nodiscard]] Kind kind() const noexcept { return static_cast<Kind>(data_.index()); }
    [[nodiscard]] bool is_null() const noexcept { return kind() == Kind::null; }

    // nullptr quando o valor é de outro tipo.
    [[nodiscard]] const bool* boolean() const noexcept { return std::get_if<bool>(&data_); }
    [[nodiscard]] const std::int64_t* integer() const noexcept { return std::get_if<std::int64_t>(&data_); }
    [[nodiscard]] const double* real() const noexcept { return std::get_if<double>(&data_); }
    [[nodiscard]] const std::string* text() const noexcept { return std::get_if<std::string>(&data_); }
    [[nodiscard]] const object::ObjectId* id() const noexcept { return std::get_if<object::ObjectId>(&data_); }
    [[nodiscard]] const ValueList* list() const noexcept { return std::get_if<ValueList>(&data_); }
    [[nodiscard]] const ValueMap* map() const noexcept { return std::get_if<ValueMap>(&data_); }
    // Campo de um mapa (nullptr se não é mapa ou não tem o campo).
    [[nodiscard]] const Value* field(std::string_view name) const;

    friend bool operator==(const Value&, const Value&) = default;

private:
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, object::ObjectId, ValueList, ValueMap>
        data_{nullptr};
};

[[nodiscard]] std::string_view kind_name(Value::Kind kind) noexcept;

// --- binário (OpCall.args / OpResult.payload) ----------------------------------

inline constexpr std::uint8_t value_encoding_version = 1;
// Aninhamento máximo aceito na leitura (binário e JSON): entrada de rede não
// pode estourar a pilha.
inline constexpr int value_max_depth = 64;

[[nodiscard]] std::vector<std::byte> encode(const Value& value);
[[nodiscard]] Result<Value> decode(std::span<const std::byte> bytes);

// --- JSON ------------------------------------------------------------------------
//
// id vira número em JSON (não há tipo id); na volta, número inteiro vira
// integer, e `Args::id` aceita integer >= 1 onde se espera um id.

[[nodiscard]] std::string to_json(const Value& value);
[[nodiscard]] Result<Value> from_json(std::string_view text);

// --- argumentos de uma proc --------------------------------------------------------
//
// Os argumentos são sempre um mapa. Leitura tipada com erro que diz qual campo e
// o que se esperava (ErrorCode::invalid_argument).
class Args {
public:
    Args() = default;
    explicit Args(ValueMap values) : values_{std::move(values)} {}
    [[nodiscard]] static Result<Args> decode(std::span<const std::byte> bytes);
    [[nodiscard]] static Result<Args> from_value(Value value);

    [[nodiscard]] bool has(std::string_view name) const;
    [[nodiscard]] const Value* get(std::string_view name) const;
    [[nodiscard]] const ValueMap& values() const noexcept { return values_; }

    // Obrigatórios: ausente ou nulo é erro.
    [[nodiscard]] Result<object::ObjectId> id(std::string_view name) const;
    [[nodiscard]] Result<std::string> text(std::string_view name) const;
    [[nodiscard]] Result<std::int64_t> integer(std::string_view name) const;
    [[nodiscard]] Result<bool> boolean(std::string_view name) const;
    [[nodiscard]] Result<std::vector<object::ObjectId>> ids(std::string_view name) const;
    // Opcionais: ausente ou nulo devolve o padrão; tipo errado continua erro.
    [[nodiscard]] Result<std::string> text_or(std::string_view name, std::string fallback) const;
    [[nodiscard]] Result<std::int64_t> integer_or(std::string_view name, std::int64_t fallback) const;
    [[nodiscard]] Result<bool> boolean_or(std::string_view name, bool fallback) const;

    [[nodiscard]] std::vector<std::byte> encode() const { return ops::encode(Value{values_}); }

private:
    ValueMap values_;
};

} // namespace modb::ops

namespace modb::object {

// Um campo `ops::Value` num objeto persistido (R14): guardado como bytes, no
// mesmo formato binário do OpCall. Serve para listas pequenas, mapas e JSON
// livre sem outro objeto nem texto para reinterpretar; não é indexável.
template <>
struct bytes_codec<ops::Value> {
    [[nodiscard]] static std::vector<std::byte> encode(const ops::Value& value) { return ops::encode(value); }
    [[nodiscard]] static Result<ops::Value> decode(std::span<const std::byte> bytes) { return ops::decode(bytes); }
};

} // namespace modb::object
