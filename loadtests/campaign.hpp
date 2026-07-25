#pragma once

// Orquestração de uma campanha `modb_load run` (docs/PLANO_TESTES_DE_CARGA.md
// §12: formato do arquivo de resultado). Reaproveita diretamente
// `benchmarks/runner/jsonl_writer`, `environment`, `sha256` e `json_util`
// (§14 do plano) -- schema próprio `modb.loadtest`, não `modb.benchmark`.

#include "budget.hpp"
#include "matrix.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace modb::loadtest {

struct CampaignOptions {
    std::optional<std::string> profile;
    MatrixSelectors selectors;
    std::filesystem::path output_dir{"load-results"};
    std::filesystem::path work_dir;   // vazio = usa output_dir
    std::filesystem::path environments_file{"loadtests/environments.json"};
    std::uint64_t seed{1};
    bool dry_run{false};
    BudgetLimits budget;
    std::string argv_joined;
};

struct CampaignResult {
    bool ok{false};
    std::string status;   // "completed" | "partial" | "failed" | "dry_run"
    std::filesystem::path result_path;
    std::string run_id;
    std::string error;
    std::string rendered_plan;   // preenchido em dry_run / erro de composição
};

// Resolve perfil+seletores em uma lista de `Case` (sem escrever nada).
struct ResolveResult {
    bool ok{false};
    std::vector<Case> cases;
    std::string error;
};
[[nodiscard]] ResolveResult resolve_cases(const CampaignOptions& options);

// Usado por `list-cases`: nunca executa nada, nunca escreve JSONL.
[[nodiscard]] std::string render_case_plan(const std::vector<Case>& cases);

[[nodiscard]] CampaignResult run_campaign(const CampaignOptions& options);

} // namespace modb::loadtest
