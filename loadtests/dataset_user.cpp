#include "dataset_user.hpp"

#include <cctype>
#include <sstream>

namespace modb::loadtest {
namespace {

// splitmix64 -- gerador determinístico simples e rápido, usado só para
// preencher bytes/campo de `status` de forma reproduzível a partir de uma
// única semente por objeto (seed ^ index), nunca de um stream compartilhado.
std::uint64_t splitmix64_next(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Base fixa (2026-01-01T00:00:00Z) + passo determinístico -- nunca relógio
// real, para que o hash lógico do dataset seja reprodutível (§7).
constexpr std::int64_t kBaseEpochSeconds = 1'767'225'600;
constexpr std::int64_t kStepSeconds = 37;

} // namespace

std::size_t filler_bytes_for_payload(std::string_view payload) {
    if (payload == "slim") {
        return 64;
    }
    if (payload == "fat") {
        return 4096;
    }
    return 256; // "normal" e qualquer valor desconhecido
}

GeneratedUser generate_user(std::uint64_t seed, std::uint64_t index, std::string_view payload) {
    GeneratedUser user;
    user.id = static_cast<std::int64_t>(index);
    user.login = "user" + std::to_string(index);
    user.email = "user" + std::to_string(index) + "@example.test";
    user.created_at = kBaseEpochSeconds + static_cast<std::int64_t>(index) * kStepSeconds;
    user.status = static_cast<std::int32_t>(index % 3);

    std::uint64_t state = seed ^ (index * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL);
    // display_name: texto sintético curto, determinístico, nunca corpus real.
    static const char* const kSyllables[] = {"an", "ber", "car", "dol", "eri",
                                             "fon", "gil", "hus", "ivo", "jor"};
    std::string name;
    const auto syllable_count = 2 + (splitmix64_next(state) % 3);
    for (std::uint64_t i = 0; i < syllable_count; ++i) {
        name += kSyllables[splitmix64_next(state) % 10];
    }
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    user.display_name = name;

    const auto filler_size = filler_bytes_for_payload(payload);
    user.filler.reserve(filler_size);
    while (user.filler.size() < filler_size) {
        const auto chunk = splitmix64_next(state);
        const auto* bytes = reinterpret_cast<const std::byte*>(&chunk);
        for (std::size_t i = 0; i < sizeof(chunk) && user.filler.size() < filler_size; ++i) {
            user.filler.push_back(bytes[i]);
        }
    }

    return user;
}

std::string canonical_line(const GeneratedUser& user) {
    std::ostringstream oss;
    oss << user.id << '|' << user.login << '|' << user.email << '|' << user.display_name << '|'
        << user.created_at << '|' << user.status << '|' << user.filler.size();
    for (const auto b : user.filler) {
        oss << ',' << static_cast<unsigned>(b);
    }
    return oss.str();
}

} // namespace modb::loadtest
