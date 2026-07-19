#pragma once

// Harness compartilhado dos alvos de fuzz (Fase 10D).
// Com libFuzzer: LLVMFuzzerTestOneInput é o entrypoint.
// Sem libFuzzer (fallback MinGW/GCC): main faz replay do corpus.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace modb::fuzz {

// Teto defensivo: entradas maiores são truncadas antes de qualquer decoder.
inline constexpr std::size_t max_fuzz_input_bytes = 1u << 20; // 1 MiB

inline std::span<const std::byte> as_bytes(const std::uint8_t* data, std::size_t size) noexcept {
    const auto n = size < max_fuzz_input_bytes ? size : max_fuzz_input_bytes;
    return {reinterpret_cast<const std::byte*>(data), n};
}

inline int replay_file(const std::filesystem::path& path,
                       int (*fn)(const std::uint8_t*, std::size_t)) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        std::cerr << "fuzz: cannot open " << path.string() << '\n';
        return 1;
    }
    std::vector<std::uint8_t> buffer(max_fuzz_input_bytes);
    in.read(reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    buffer.resize(got);
    (void)fn(buffer.data(), buffer.size());
    return 0;
}

inline int replay_corpus(int argc, char** argv, int (*fn)(const std::uint8_t*, std::size_t)) {
    if (argc < 2) {
        std::cerr << "usage: <fuzz_target> <corpus-file-or-dir>...\n";
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path path{argv[i]};
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator{path, ec}) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                failures += replay_file(entry.path(), fn);
            }
        } else if (std::filesystem::is_regular_file(path, ec)) {
            failures += replay_file(path, fn);
        } else {
            std::cerr << "fuzz: skip missing path " << path.string() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

} // namespace modb::fuzz

#if !defined(MODB_FUZZER_LIBFUZZER)
#define MODB_FUZZ_DEFINE_MAIN(TargetFn)                                                            \
    int main(int argc, char** argv) {                                                              \
        return ::modb::fuzz::replay_corpus(argc, argv, TargetFn);                                  \
    }
#else
#define MODB_FUZZ_DEFINE_MAIN(TargetFn)
#endif
