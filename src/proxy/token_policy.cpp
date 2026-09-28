#include "modb/proxy/token_policy.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace modb::proxy {
namespace {

// SHA-256 (FIPS 180-4).
constexpr std::array<std::uint32_t, 64> k_round = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t rotr(std::uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }

void compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t{block[i * 4]} << 24) | (std::uint32_t{block[i * 4 + 1]} << 16) |
               (std::uint32_t{block[i * 4 + 2]} << 8) | std::uint32_t{block[i * 4 + 3]};
    }
    for (int i = 16; i < 64; ++i) {
        const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = state;
    for (int i = 0; i < 64; ++i) {
        const auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const auto ch = (e & f) ^ (~e & g);
        const auto t1 = h + s1 + ch + k_round[i] + w[i];
        const auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const auto maj = (a & b) ^ (a & c) ^ (b & c);
        const auto t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

std::string_view trim(std::string_view s) {
    constexpr std::string_view blanks = " \t\r\n";
    const auto first = s.find_first_not_of(blanks);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(blanks) - first + 1);
}

std::optional<Sha256> from_hex(std::string_view hex) {
    if (hex.size() != 64) {
        return std::nullopt;
    }
    Sha256 out{};
    for (std::size_t i = 0; i < 32; ++i) {
        const auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            return -1;
        };
        const int hi = nibble(hex[i * 2]);
        const int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return out;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(separator, start);
        const auto piece = trim(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (!piece.empty()) {
            out.emplace_back(piece);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

} // namespace

Sha256 sha256(std::span<const std::byte> data) noexcept {
    std::array<std::uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t offset = 0;
    for (; offset + 64 <= data.size(); offset += 64) {
        compress(state, bytes + offset);
    }
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rest = data.size() - offset;
    if (rest > 0) {
        std::memcpy(tail.data(), bytes + offset, rest);
    }
    tail[rest] = 0x80;
    const std::size_t tail_blocks = rest + 1 + 8 <= 64 ? 1 : 2;
    const std::uint64_t bits = std::uint64_t{data.size()} * 8;
    for (int i = 0; i < 8; ++i) {
        tail[tail_blocks * 64 - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    for (std::size_t block = 0; block < tail_blocks; ++block) {
        compress(state, tail.data() + block * 64);
    }
    Sha256 digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
    }
    return digest;
}

Sha256 sha256(std::string_view text) noexcept {
    return sha256(std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()), text.size()});
}

std::string to_hex(const Sha256& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const auto byte : digest) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

std::string token_line(std::string_view token, std::string_view principal, std::span<const std::string> roles) {
    std::string line = "sha256:" + to_hex(sha256(token)) + " " + std::string{principal};
    for (std::size_t i = 0; i < roles.size(); ++i) {
        line += (i == 0 ? " " : ",") + roles[i];
    }
    return line;
}

Result<TokenStore> TokenStore::parse(std::string_view text) {
    TokenStore store;
    std::size_t number = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        start = end == std::string_view::npos ? text.size() : end + 1;
        ++number;
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) {
            continue;
        }
        const auto where = "tokens line " + std::to_string(number) + ": ";
        const auto words = split(line, ' ');
        if (words.size() < 2 || words.size() > 3 || !words[0].starts_with("sha256:")) {
            return std::unexpected(Error{ErrorCode::invalid_argument,
                                         where + "expected 'sha256:<hex> principal [role,...]'"});
        }
        auto hash = from_hex(std::string_view{words[0]}.substr(7));
        if (!hash) {
            return std::unexpected(Error{ErrorCode::invalid_argument, where + "hash must be 64 hex digits"});
        }
        store.entries_.push_back(Entry{.hash = *hash,
                                       .principal = words[1],
                                       .roles = words.size() == 3 ? split(words[2], ',') : std::vector<std::string>{}});
    }
    return store;
}

Result<TokenStore> TokenStore::load(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return std::unexpected(Error{ErrorCode::file_not_found, "cannot read tokens file: " + file.string()});
    }
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    return parse(text);
}

std::optional<ops::Caller> TokenStore::find(std::span<const std::byte> token) const {
    const auto digest = sha256(token);
    const Entry* match = nullptr;
    // Compara com todas: o tempo não diz qual entrada (nem se alguma) bateu.
    for (const auto& entry : entries_) {
        std::uint8_t diff = 0;
        for (std::size_t i = 0; i < digest.size(); ++i) {
            diff |= static_cast<std::uint8_t>(digest[i] ^ entry.hash[i]);
        }
        if (diff == 0 && match == nullptr) {
            match = &entry;
        }
    }
    if (match == nullptr) {
        return std::nullopt;
    }
    return ops::Caller{.principal = match->principal, .roles = match->roles};
}

TokenPolicy::TokenPolicy(TokenStore tokens, std::shared_ptr<Policy> inner)
    : tokens_{std::move(tokens)}, inner_{inner ? std::move(inner) : std::make_shared<PassThroughPolicy>()},
      name_{"token+" + std::string{inner_->name()}} {}

Result<ops::Caller> TokenPolicy::authenticate(const ClientInfo& client, const Credentials& credentials) {
    if (credentials.mechanism != "token") {
        return std::unexpected(Error{ErrorCode::unauthenticated, "this proxy requires the token mechanism"});
    }
    auto caller = tokens_.find(credentials.payload);
    if (!caller) {
        return std::unexpected(Error{ErrorCode::unauthenticated, "unknown token"});
    }
    caller->attributes.emplace_back("address", client.address);
    caller->attributes.emplace_back("tls", client.tls ? "1" : "0");
    caller->attributes.emplace_back("auth", "token");
    return std::move(*caller);
}

Decision TokenPolicy::authorize(const ops::Caller& caller, net::Message& request) {
    return inner_->authorize(caller, request);
}

void TokenPolicy::on_response(const ops::Caller& caller, net::Message& response) { inner_->on_response(caller, response); }

void TokenPolicy::audit(const AuditRecord& record) { inner_->audit(record); }

} // namespace modb::proxy
