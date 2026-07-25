// Lesson 20 -- Performance and Hardening.
// Final lesson: points the tutorial code at the product's benchmark, load,
// and fuzzing evidence.
// See docs/training/en/20-performance-and-hardening/20-performance-and-hardening.md.

#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path repo_root() {
    return std::filesystem::path{MODB_TRAINING_DIR}.parent_path().parent_path().parent_path();
}

bool require(const std::filesystem::path& root, std::string_view relative) {
    const auto path = root / std::filesystem::path{relative};
    const bool exists = std::filesystem::exists(path);
    std::cout << (exists ? "Found " : "Missing ") << relative << '\n';
    return exists;
}

} // namespace

int main() {
    std::cout << "Objective: connect training code to benchmark, load, and fuzz evidence.\n";
    const auto root = repo_root();

    bool ok = true;
    ok = require(root, "docs/PLANO_BENCHMARKS.md") && ok;
    ok = require(root, "docs/PLANO_TESTES_DE_CARGA.md") && ok;
    ok = require(root, "docs/FUZZING.md") && ok;
    ok = require(root, "scripts/run-benchmarks.ps1") && ok;
    ok = require(root, "scripts/run-load.ps1") && ok;
    ok = require(root, "tests/fuzz/corpus") && ok;

    std::cout << "Next commands:\n";
    std::cout << "  modb_bench run --profile smoke\n";
    std::cout << "  scripts/run-load.ps1 -Config loadtests/config/load-local.yaml -DryRun\n";
    std::cout << "  ctest -R modb.fuzz\n";

    return ok ? 0 : 1;
}
