#include "json_value.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <optional>

namespace modb::loadtest {
namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonParseResult parse() {
        JsonParseResult result;
        skip_ws();
        auto value = parse_value();
        if (!value) {
            result.error = error_;
            return result;
        }
        skip_ws();
        if (pos_ != text_.size()) {
            result.error = position_prefix() + "conteúdo à direita do documento JSON";
            return result;
        }
        result.ok = true;
        result.value = std::move(*value);
        return result;
    }

private:
    std::string_view text_;
    std::size_t pos_{0};
    std::string error_;

    [[nodiscard]] bool eof() const { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const { return text_[pos_]; }

    [[nodiscard]] std::string position_prefix() const {
        std::size_t line = 1;
        std::size_t col = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        return std::to_string(line) + ":" + std::to_string(col) + ": ";
    }

    void skip_ws() {
        while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            ++pos_;
        }
    }

    bool expect(char c) {
        if (eof() || peek() != c) {
            error_ = position_prefix() + "esperava '" + std::string(1, c) + "'";
            return false;
        }
        ++pos_;
        return true;
    }

    std::optional<JsonValue> parse_value() {
        if (eof()) {
            error_ = position_prefix() + "fim inesperado do documento";
            return std::nullopt;
        }
        switch (peek()) {
            case '{':
                return parse_object();
            case '[':
                return parse_array();
            case '"':
                return parse_string_value();
            case 't':
            case 'f':
                return parse_bool();
            case 'n':
                return parse_null();
            default:
                return parse_number();
        }
    }

    std::optional<JsonValue> parse_object() {
        if (!expect('{')) {
            return std::nullopt;
        }
        JsonObject obj;
        skip_ws();
        if (!eof() && peek() == '}') {
            ++pos_;
            return JsonValue{std::move(obj)};
        }
        while (true) {
            skip_ws();
            auto key = parse_string();
            if (!key) {
                return std::nullopt;
            }
            skip_ws();
            if (!expect(':')) {
                return std::nullopt;
            }
            skip_ws();
            auto value = parse_value();
            if (!value) {
                return std::nullopt;
            }
            obj.emplace_back(std::move(*key), std::move(*value));
            skip_ws();
            if (eof()) {
                error_ = position_prefix() + "objeto não fechado";
                return std::nullopt;
            }
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                break;
            }
            error_ = position_prefix() + "esperava ',' ou '}'";
            return std::nullopt;
        }
        return JsonValue{std::move(obj)};
    }

    std::optional<JsonValue> parse_array() {
        if (!expect('[')) {
            return std::nullopt;
        }
        JsonArray arr;
        skip_ws();
        if (!eof() && peek() == ']') {
            ++pos_;
            return JsonValue{std::move(arr)};
        }
        while (true) {
            skip_ws();
            auto value = parse_value();
            if (!value) {
                return std::nullopt;
            }
            arr.push_back(std::move(*value));
            skip_ws();
            if (eof()) {
                error_ = position_prefix() + "array não fechado";
                return std::nullopt;
            }
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                break;
            }
            error_ = position_prefix() + "esperava ',' ou ']'";
            return std::nullopt;
        }
        return JsonValue{std::move(arr)};
    }

    std::optional<std::string> parse_string() {
        if (!expect('"')) {
            return std::nullopt;
        }
        std::string out;
        while (true) {
            if (eof()) {
                error_ = position_prefix() + "string não fechada";
                return std::nullopt;
            }
            char c = text_[pos_++];
            if (c == '"') {
                break;
            }
            if (c == '\\') {
                if (eof()) {
                    error_ = position_prefix() + "escape incompleto";
                    return std::nullopt;
                }
                char esc = text_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > text_.size()) {
                            error_ = position_prefix() + "\\u incompleto";
                            return std::nullopt;
                        }
                        unsigned code = 0;
                        auto res = std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, code, 16);
                        if (res.ec != std::errc{}) {
                            error_ = position_prefix() + "\\uXXXX inválido";
                            return std::nullopt;
                        }
                        pos_ += 4;
                        // Só o plano básico multilíngue é suportado (sem pares
                        // substitutos) -- suficiente para o catálogo de
                        // ambientes, que não carrega texto fora do BMP.
                        if (code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default:
                        error_ = position_prefix() + "escape desconhecido";
                        return std::nullopt;
                }
                continue;
            }
            out.push_back(c);
        }
        return out;
    }

    std::optional<JsonValue> parse_string_value() {
        auto s = parse_string();
        if (!s) {
            return std::nullopt;
        }
        return JsonValue{std::move(*s)};
    }

    std::optional<JsonValue> parse_bool() {
        if (text_.substr(pos_, 4) == "true") {
            pos_ += 4;
            return JsonValue{true};
        }
        if (text_.substr(pos_, 5) == "false") {
            pos_ += 5;
            return JsonValue{false};
        }
        error_ = position_prefix() + "literal booleano inválido";
        return std::nullopt;
    }

    std::optional<JsonValue> parse_null() {
        if (text_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return JsonValue{nullptr};
        }
        error_ = position_prefix() + "literal 'null' inválido";
        return std::nullopt;
    }

    std::optional<JsonValue> parse_number() {
        const std::size_t start = pos_;
        if (!eof() && peek() == '-') {
            ++pos_;
        }
        while (!eof() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.' ||
                          peek() == 'e' || peek() == 'E' || peek() == '+' || peek() == '-')) {
            ++pos_;
        }
        if (pos_ == start) {
            error_ = position_prefix() + "token inesperado";
            return std::nullopt;
        }
        double value = std::strtod(std::string(text_.substr(start, pos_ - start)).c_str(), nullptr);
        return JsonValue{value};
    }
};

} // namespace

JsonParseResult parse_json(std::string_view text) {
    Parser parser{text};
    return parser.parse();
}

} // namespace modb::loadtest
