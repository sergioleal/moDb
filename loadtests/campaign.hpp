#pragma once

// Orquestração de uma campanha `modb_load run` (docs/PLANO_TESTES_DE_CARGA.md
// §12: formato do arquivo de resultado). Reaproveita diretamente
// `benchmarks/runner/jsonl_writer`, `environment`, `sha256` e `json_util`
// (§14 do plano) -- schema próprio `modb.loadtest`, não `modb.benchmark`.

#include "budget.hpp"
#include "calibration.hpp"
#include "matrix.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace modb::loadtest {

// Um `case_error` cujo texto é uma dessas frases não é uma medição -- é uma
// recusa de execução (workload sem dispatch nenhum, ou dispatch existente mas
// que não cobre o alvo/dimensão pedido) já declarada como dívida conhecida
// (docs-process/PLANO_IMPLEMENTACAO_CARGA.md, Subfase C). `resume_campaign`
// e `rollup.cpp` compartilham esta checagem -- duas fontes de verdade
// divergentes já causaram um caso real ser contado como "failed" (Subfase R,
// revisão pós-implementação).
[[nodiscard]] bool is_unimplemented_error_text(const std::string& error);

struct CampaignOptions {
    std::optional<std::string> profile;
    MatrixSelectors selectors;
    std::filesystem::path output_dir{"load-results"};
    std::filesystem::path work_dir;   // vazio = usa output_dir
    std::filesystem::path environments_file{"loadtests/environments.json"};
    std::filesystem::path calibration_file;   // vazio = usa default_calibration_path()
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

// Usado por `list-cases`/`run --dry-run`: nunca executa nada, nunca escreve
// JSONL. `table` vazia (sem arquivo de calibração) faz toda estimativa
// imprimir "?", igual ao comportamento anterior à Subfase H.
[[nodiscard]] std::string render_case_plan(const std::vector<Case>& cases,
                                           const CalibrationTable& table);

[[nodiscard]] CampaignResult run_campaign(const CampaignOptions& options);

// Subfase F (§6.4 "Retomada"): reabre um `.partial` interrompido, reconstrói
// os casos já concluídos (case_summary ou case_error) a partir do próprio
// arquivo -- sem depender de nenhum estado externo -- e executa só o
// restante do `case_plan`, no mesmo arquivo. `work_dir` não é persistido no
// schema (§12); por padrão usa o diretório do próprio `.partial` (igual ao
// comportamento padrão de `run`, que também usa `output_dir` quando
// `--work-dir` não é informado).
struct ResumeOptions {
    std::filesystem::path partial_path;
    std::filesystem::path work_dir;
    std::uint64_t seed_override{0};   // 0 = usa o seed gravado no run_start
};

[[nodiscard]] CampaignResult resume_campaign(const ResumeOptions& options);

} // namespace modb::loadtest
