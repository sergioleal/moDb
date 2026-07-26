// Lesson 21 -- Load Testing and Dashboard.
// Shows the operator workflow for modb_load without launching a long run.
// See docs/training/en/21-load-testing-dashboard/21-load-testing-dashboard.md.

#include <filesystem>
#include <iostream>
#include <string_view>

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
    std::cout << "Objective: use modb_load and inspect results in the dashboard.\n";
    const auto root = repo_root();

    bool ok = true;
    ok = require(root, "loadtests/config/load-smoke.yaml") && ok;
    ok = require(root, "loadtests/dashboard/index.html") && ok;
    ok = require(root, "loadtests/environments.json") && ok;
    ok = require(root, "docs/PLANO_TESTES_DE_CARGA.md") && ok;

    std::cout << "Tool target to build: modb_load\n";
    std::cout << "Suggested workflow:\n";
    std::cout << "  modb_load list-profiles\n";
    std::cout << "  modb_load list-cases --profile load-smoke --workload create_only --target embedded\n";
    std::cout << "  modb_load run --profile load-smoke --workload create_only --target embedded --scale 1k --accept-unknown-budget\n";
    std::cout << "  modb_load index <result.jsonl> --history-file load-history/series.jsonl\n";
    std::cout << "  open loadtests/dashboard/index.html and load load-history/series.jsonl\n";

    return ok ? 0 : 1;
}
