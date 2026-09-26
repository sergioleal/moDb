#pragma once

// Execuções excluídas das análises da série histórica (T18 do plano de
// desempenho). A série é append-only (docs/PLANO_TESTES_DE_CARGA.md §13.2):
// um ponto medido sob contaminação conhecida não é apagado, mas também não pode
// entrar em tendência nem em gate. A lista mora ao lado da série, versionada:
//
//   load-history/excluded_runs.json
//   {"schema":"modb.loadtest.excluded_runs","schema_version":1,
//    "runs":[{"run_id":"run-...","case_id":"load....","reason":"..."}]}
//
// `case_id` é opcional: sem ele, todos os casos da execução saem; com ele, só
// aquele (uma campanha roda vários casos, e só um pode estar contaminado).
// Arquivo ausente = nada excluído.

#include <filesystem>
#include <string>
#include <unordered_map>

namespace modb::loadtest {

struct ExcludedRuns {
    // "run_id" (a execução inteira) ou "run_id|case_id" -> motivo
    std::unordered_map<std::string, std::string> reasons;
    std::string error;   // não vazio = arquivo presente mas ilegível

    [[nodiscard]] bool contains(const std::string& run_id, const std::string& case_id) const {
        return reasons.contains(run_id) || reasons.contains(run_id + "|" + case_id);
    }
};

// Lê `excluded_runs.json` do diretório de `history_path`.
[[nodiscard]] ExcludedRuns load_excluded_runs(const std::filesystem::path& history_path);

} // namespace modb::loadtest
