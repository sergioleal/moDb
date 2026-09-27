#include "modb/ops/value.hpp"

#include "modb/storage/binary.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace modb::ops {

namespace {

Error invalid(std::string message) { return Error{ErrorCode::invalid_argument, std::move(message)}; }
Error corrupt(std::string message) { return Error{ErrorCode::invalid_encoding, "value: " + std::move(message)}; }

} // namespace

const Value* Value::field(std::string_view name) const {
    const auto* m = map();
    if (m == nullptr) {
        return nullptr;
    }
    const auto it = m->find(name);
    return it == m->end() ? nullptr : &it->second;
}

std::string_view kind_name(Value::Kind kind) noexcept {
    switch (kind) {
    case Value::Kind::null: return "null";
    case Value::Kind::boolean: return "boolean";
    case Value::Kind::integer: return "integer";
    case Value::Kind::real: return "real";
    case Value::Kind::text: return "text";
    case Value::Kind::id: return "id";
    case Value::Kind::list: return "list";
    case Value::Kind::map: return "map";
    }
    return "?";
}

// --- binário ----------------------------------------------------------------------

namespace {

void write_value(storage::BinaryWriter& w, const Value& v) {
    w.write_u8(static_cast<std::uint8_t>(v.kind()));
    switch (v.kind()) {
    case Value::Kind::null: break;
    case Value::Kind::boolean: w.write_u8(*v.boolean() ? 1 : 0); break;
    case Value::Kind::integer: w.write_u64(static_cast<std::uint64_t>(*v.integer())); break;
    case Value::Kind::real: w.write_u64(std::bit_cast<std::uint64_t>(*v.real())); break;
    case Value::Kind::text: {
        const auto& t = *v.text();
        w.write_u32(static_cast<std::uint32_t>(t.size()));
        w.write_bytes(std::as_bytes(std::span{t.data(), t.size()}));
        break;
    }
    case Value::Kind::id: w.write_u64(v.id()->value); break;
    case Value::Kind::list: {
        const auto& l = *v.list();
        w.write_u32(static_cast<std::uint32_t>(l.size()));
        for (const auto& item : l) {
            write_value(w, item);
        }
        break;
    }
    case Value::Kind::map: {
        const auto& m = *v.map();
        w.write_u32(static_cast<std::uint32_t>(m.size()));
        for (const auto& [k, item] : m) {
            w.write_u32(static_cast<std::uint32_t>(k.size()));
            w.write_bytes(std::as_bytes(std::span{k.data(), k.size()}));
            write_value(w, item);
        }
        break;
    }
    }
}

Result<std::string> read_text(storage::BinaryReader& r) {
    auto size = r.read_u32();
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size > r.remaining()) {
        return std::unexpected(corrupt("text longer than the input"));
    }
    auto bytes = r.read_bytes(*size);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

Result<Value> read_value(storage::BinaryReader& r, int depth) {
    if (depth > value_max_depth) {
        return std::unexpected(corrupt("nested too deep"));
    }
    auto tag = r.read_u8();
    if (!tag) {
        return std::unexpected(tag.error());
    }
    switch (static_cast<Value::Kind>(*tag)) {
    case Value::Kind::null: return Value{};
    case Value::Kind::boolean: {
        auto b = r.read_u8();
        if (!b) {
            return std::unexpected(b.error());
        }
        if (*b > 1) {
            return std::unexpected(corrupt("boolean must be 0 or 1"));
        }
        return Value{*b == 1};
    }
    case Value::Kind::integer: {
        auto u = r.read_u64();
        if (!u) {
            return std::unexpected(u.error());
        }
        return Value{static_cast<std::int64_t>(*u)};
    }
    case Value::Kind::real: {
        auto u = r.read_u64();
        if (!u) {
            return std::unexpected(u.error());
        }
        return Value{std::bit_cast<double>(*u)};
    }
    case Value::Kind::text: {
        auto t = read_text(r);
        if (!t) {
            return std::unexpected(t.error());
        }
        return Value{std::move(*t)};
    }
    case Value::Kind::id: {
        auto u = r.read_u64();
        if (!u) {
            return std::unexpected(u.error());
        }
        return Value{object::ObjectId{*u}};
    }
    case Value::Kind::list: {
        auto count = r.read_u32();
        if (!count) {
            return std::unexpected(count.error());
        }
        // Cada item ocupa ao menos 1 byte (a tag): contagem maior que a entrada é corrupção.
        if (*count > r.remaining()) {
            return std::unexpected(corrupt("list count larger than the input"));
        }
        ValueList items;
        items.reserve(*count);
        for (std::uint32_t i = 0; i < *count; ++i) {
            auto item = read_value(r, depth + 1);
            if (!item) {
                return std::unexpected(item.error());
            }
            items.push_back(std::move(*item));
        }
        return Value{std::move(items)};
    }
    case Value::Kind::map: {
        auto count = r.read_u32();
        if (!count) {
            return std::unexpected(count.error());
        }
        if (*count > r.remaining()) {
            return std::unexpected(corrupt("map count larger than the input"));
        }
        ValueMap items;
        for (std::uint32_t i = 0; i < *count; ++i) {
            auto key = read_text(r);
            if (!key) {
                return std::unexpected(key.error());
            }
            auto item = read_value(r, depth + 1);
            if (!item) {
                return std::unexpected(item.error());
            }
            if (!items.emplace(std::move(*key), std::move(*item)).second) {
                return std::unexpected(corrupt("duplicate map key"));
            }
        }
        return Value{std::move(items)};
    }
    }
    return std::unexpected(corrupt("unknown tag " + std::to_string(*tag)));
}

} // namespace

std::vector<std::byte> encode(const Value& value) {
    storage::BinaryWriter w;
    w.write_u8(value_encoding_version);
    write_value(w, value);
    return std::move(w).take();
}

Result<Value> decode(std::span<const std::byte> bytes) {
    storage::BinaryReader r{bytes};
    auto version = r.read_u8();
    if (!version) {
        return std::unexpected(corrupt("empty input"));
    }
    if (*version != value_encoding_version) {
        return std::unexpected(corrupt("unsupported encoding version " + std::to_string(*version)));
    }
    auto value = read_value(r, 0);
    if (!value) {
        return value.error().code == ErrorCode::invalid_encoding ? value
                                                                 : std::unexpected(corrupt(value.error().message));
    }
    if (!r.at_end()) {
        return std::unexpected(corrupt("trailing bytes after the value"));
    }
    return value;
}

// --- JSON -----------------------------------------------------------------------------

namespace {

void json_text(std::string& out, std::string_view s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void json_value(std::string& out, const Value& v) {
    switch (v.kind()) {
    case Value::Kind::null: out += "null"; break;
    case Value::Kind::boolean: out += *v.boolean() ? "true" : "false"; break;
    case Value::Kind::integer: out += std::to_string(*v.integer()); break;
    case Value::Kind::id: out += std::to_string(v.id()->value); break;
    case Value::Kind::real: {
        const double d = *v.real();
        if (!std::isfinite(d)) {
            out += "null";  // JSON não representa NaN nem infinito
            break;
        }
        char buf[32];
        const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, d);
        std::string_view s{buf, ec == std::errc{} ? static_cast<std::size_t>(end - buf) : 0};
        out += s;
        // Mantém o tipo real na ida e volta: 2.0 sai "2.0", não "2".
        if (s.find_first_of(".eE") == std::string_view::npos) {
            out += ".0";
        }
        break;
    }
    case Value::Kind::text: json_text(out, *v.text()); break;
    case Value::Kind::list: {
        out += '[';
        bool first = true;
        for (const auto& item : *v.list()) {
            if (!first) {
                out += ',';
            }
            first = false;
            json_value(out, item);
        }
        out += ']';
        break;
    }
    case Value::Kind::map: {
        out += '{';
        bool first = true;
        for (const auto& [k, item] : *v.map()) {
            if (!first) {
                out += ',';
            }
            first = false;
            json_text(out, k);
            out += ':';
            json_value(out, item);
        }
        out += '}';
        break;
    }
    }
}

class JsonReader {
public:
    explicit JsonReader(std::string_view s) : s_{s} {}

    Result<Value> document() {
        auto v = value(0);
        if (!v) {
            return v;
        }
        skip();
        if (pos_ != s_.size()) {
            return fail("content after the value");
        }
        return v;
    }

private:
    std::string_view s_;
    std::size_t pos_{0};

    std::unexpected<Error> fail(std::string_view why) const {
        return std::unexpected(invalid("invalid JSON at " + std::to_string(pos_) + ": " + std::string{why}));
    }
    void skip() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }
    bool word(std::string_view w) {
        if (s_.substr(pos_, w.size()) == w) {
            pos_ += w.size();
            return true;
        }
        return false;
    }

    Result<Value> value(int depth) {
        if (depth > value_max_depth) {
            return fail("nested too deep");
        }
        skip();
        if (pos_ >= s_.size()) {
            return fail("unexpected end");
        }
        const char c = s_[pos_];
        if (c == '{') {
            return object(depth);
        }
        if (c == '[') {
            return array(depth);
        }
        if (c == '"') {
            auto t = string();
            if (!t) {
                return std::unexpected(t.error());
            }
            return Value{std::move(*t)};
        }
        if (word("true")) {
            return Value{true};
        }
        if (word("false")) {
            return Value{false};
        }
        if (word("null")) {
            return Value{};
        }
        return number();
    }

    Result<Value> object(int depth) {
        ++pos_;
        ValueMap items;
        skip();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return Value{std::move(items)};
        }
        while (true) {
            skip();
            if (pos_ >= s_.size() || s_[pos_] != '"') {
                return fail("expected a field name");
            }
            auto key = string();
            if (!key) {
                return std::unexpected(key.error());
            }
            skip();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                return fail("expected ':'");
            }
            ++pos_;
            auto v = value(depth + 1);
            if (!v) {
                return v;
            }
            items.insert_or_assign(std::move(*key), std::move(*v));
            skip();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return Value{std::move(items)};
            }
            return fail("expected ',' or '}'");
        }
    }

    Result<Value> array(int depth) {
        ++pos_;
        ValueList items;
        skip();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return Value{std::move(items)};
        }
        while (true) {
            auto v = value(depth + 1);
            if (!v) {
                return v;
            }
            items.push_back(std::move(*v));
            skip();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return Value{std::move(items)};
            }
            return fail("expected ',' or ']'");
        }
    }

    // Número inteiro (sem '.', 'e') e dentro de int64 vira integer; o resto, real.
    Result<Value> number() {
        const std::size_t start = pos_;
        bool is_real = false;
        if (pos_ < s_.size() && s_[pos_] == '-') {
            ++pos_;
        }
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c >= '0' && c <= '9') {
                ++pos_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || (c == '-' && pos_ > start)) {
                is_real = true;
                ++pos_;
            } else {
                break;
            }
        }
        const auto text = s_.substr(start, pos_ - start);
        if (text.empty() || text == "-") {
            pos_ = start;
            return fail("unexpected value");
        }
        if (!is_real) {
            std::int64_t i = 0;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), i);
            if (ec == std::errc{} && end == text.data() + text.size()) {
                return Value{i};
            }
        }
        double d = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), d);
        if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(d)) {
            pos_ = start;
            return fail("invalid number");
        }
        return Value{d};
    }

    static void utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    Result<std::uint32_t> hex4() {
        if (pos_ + 4 > s_.size()) {
            return fail("incomplete \\u escape");
        }
        std::uint32_t v = 0;
        const auto [end, ec] = std::from_chars(s_.data() + pos_, s_.data() + pos_ + 4, v, 16);
        if (ec != std::errc{} || end != s_.data() + pos_ + 4) {
            return fail("invalid \\u escape");
        }
        pos_ += 4;
        return v;
    }

    Result<std::string> string() {
        ++pos_;
        std::string out;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control character inside a string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) {
                break;
            }
            switch (s_[pos_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto cp = hex4();
                if (!cp) {
                    return std::unexpected(cp.error());
                }
                if (*cp >= 0xD800 && *cp <= 0xDBFF && s_.substr(pos_, 2) == "\\u") {
                    pos_ += 2;
                    auto low = hex4();
                    if (!low) {
                        return std::unexpected(low.error());
                    }
                    if (*low < 0xDC00 || *low > 0xDFFF) {
                        return fail("invalid surrogate pair");
                    }
                    *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*low - 0xDC00);
                }
                utf8(out, *cp);
                break;
            }
            default: return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }
};

} // namespace

std::string to_json(const Value& value) {
    std::string out;
    json_value(out, value);
    return out;
}

Result<Value> from_json(std::string_view text) { return JsonReader{text}.document(); }

// --- Args --------------------------------------------------------------------------

namespace {

Error missing(std::string_view name) { return invalid("argument '" + std::string{name} + "' is required"); }
Error wrong(std::string_view name, std::string_view expected, const Value& got) {
    return invalid("argument '" + std::string{name} + "' must be " + std::string{expected} + ", got " +
                   std::string{kind_name(got.kind())});
}

// Um id vem como id (cliente C++) ou como inteiro >= 1 (JSON).
Result<object::ObjectId> as_id(std::string_view name, const Value& v) {
    if (const auto* id = v.id()) {
        return *id;
    }
    if (const auto* i = v.integer(); i != nullptr && *i >= 1) {
        return object::ObjectId{static_cast<std::uint64_t>(*i)};
    }
    return std::unexpected(wrong(name, "an object id", v));
}

} // namespace

Result<Args> Args::decode(std::span<const std::byte> bytes) {
    // Sem bytes = sem argumentos (proc sem parâmetros).
    if (bytes.empty()) {
        return Args{};
    }
    auto v = ops::decode(bytes);
    if (!v) {
        return std::unexpected(v.error());
    }
    return from_value(std::move(*v));
}

Result<Args> Args::from_value(Value value) {
    if (value.is_null()) {
        return Args{};
    }
    const auto* m = value.map();
    if (m == nullptr) {
        return std::unexpected(invalid("arguments must be a map, got " + std::string{kind_name(value.kind())}));
    }
    return Args{*m};
}

bool Args::has(std::string_view name) const {
    const auto* v = get(name);
    return v != nullptr && !v->is_null();
}

const Value* Args::get(std::string_view name) const {
    const auto it = values_.find(name);
    return it == values_.end() ? nullptr : &it->second;
}

Result<object::ObjectId> Args::id(std::string_view name) const {
    const auto* v = get(name);
    if (v == nullptr || v->is_null()) {
        return std::unexpected(missing(name));
    }
    return as_id(name, *v);
}

Result<std::string> Args::text(std::string_view name) const {
    const auto* v = get(name);
    if (v == nullptr || v->is_null()) {
        return std::unexpected(missing(name));
    }
    if (const auto* t = v->text()) {
        return *t;
    }
    return std::unexpected(wrong(name, "text", *v));
}

Result<std::int64_t> Args::integer(std::string_view name) const {
    const auto* v = get(name);
    if (v == nullptr || v->is_null()) {
        return std::unexpected(missing(name));
    }
    if (const auto* i = v->integer()) {
        return *i;
    }
    return std::unexpected(wrong(name, "an integer", *v));
}

Result<bool> Args::boolean(std::string_view name) const {
    const auto* v = get(name);
    if (v == nullptr || v->is_null()) {
        return std::unexpected(missing(name));
    }
    if (const auto* b = v->boolean()) {
        return *b;
    }
    return std::unexpected(wrong(name, "true or false", *v));
}

Result<std::vector<object::ObjectId>> Args::ids(std::string_view name) const {
    const auto* v = get(name);
    if (v == nullptr || v->is_null()) {
        return std::unexpected(missing(name));
    }
    const auto* l = v->list();
    if (l == nullptr) {
        return std::unexpected(wrong(name, "a list of object ids", *v));
    }
    std::vector<object::ObjectId> out;
    out.reserve(l->size());
    for (const auto& item : *l) {
        auto id = as_id(name, item);
        if (!id) {
            return std::unexpected(id.error());
        }
        out.push_back(*id);
    }
    return out;
}

Result<std::string> Args::text_or(std::string_view name, std::string fallback) const {
    return has(name) ? text(name) : Result<std::string>{std::move(fallback)};
}

Result<std::int64_t> Args::integer_or(std::string_view name, std::int64_t fallback) const {
    return has(name) ? integer(name) : Result<std::int64_t>{fallback};
}

Result<bool> Args::boolean_or(std::string_view name, bool fallback) const {
    return has(name) ? boolean(name) : Result<bool>{fallback};
}

} // namespace modb::ops
