#pragma once

// Parser JSON mínimo, só leitura, sem dependências externas. O restante do
// projeto só tinha serialização (benchmarks/runner/json_util.hpp) e extração
// ad hoc por substring (campaign.cpp) -- nada que leia um documento aninhado
// como loadtests/environments.json. Não é um parser de propósito geral
// otimizado; é suficiente para configs pequenas lidas uma vez.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace modb::loadtest {

class JsonValue;
using JsonArray = std::vector<JsonValue>;
// Ordem de inserção preservada (map ordenado por chave seria suficiente aqui,
// mas um vetor de pares evita qualquer ambiguidade de ordem em erro).
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

class JsonValue {
public:
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;

    JsonValue() : storage_(nullptr) {}
    JsonValue(Storage storage) : storage_(std::move(storage)) {}

    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::nullptr_t>(storage_); }
    [[nodiscard]] bool is_object() const { return std::holds_alternative<JsonObject>(storage_); }
    [[nodiscard]] bool is_array() const { return std::holds_alternative<JsonArray>(storage_); }
    [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(storage_); }
    [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(storage_); }
    [[nodiscard]] bool is_number() const { return std::holds_alternative<double>(storage_); }

    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(storage_); }
    [[nodiscard]] bool as_bool() const { return std::get<bool>(storage_); }
    [[nodiscard]] double as_number() const { return std::get<double>(storage_); }
    [[nodiscard]] const JsonArray& as_array() const { return std::get<JsonArray>(storage_); }
    [[nodiscard]] const JsonObject& as_object() const { return std::get<JsonObject>(storage_); }

    // nullptr se a chave não existir ou `this` não for objeto.
    [[nodiscard]] const JsonValue* find(std::string_view key) const {
        if (!is_object()) {
            return nullptr;
        }
        for (const auto& [k, v] : as_object()) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }

    // "" se a chave não existir, não for objeto, ou o valor não for string.
    [[nodiscard]] std::string get_string(std::string_view key, std::string fallback = "") const {
        const auto* v = find(key);
        return (v && v->is_string()) ? v->as_string() : fallback;
    }

private:
    Storage storage_;
};

struct JsonParseResult {
    bool ok{false};
    JsonValue value;
    std::string error;   // "linha:coluna: mensagem" quando !ok
};

// Parseia um documento JSON UTF-8 completo. Não tolera lixo à direita (§ da
// mesma disciplina usada no resto do projeto: erro de formato é falha, não
// melhor esforço).
[[nodiscard]] JsonParseResult parse_json(std::string_view text);

} // namespace modb::loadtest
