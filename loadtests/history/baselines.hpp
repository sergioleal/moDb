#pragma once

// Baselines marcadas (docs/PLANO_TESTES_DE_CARGA.md §13.9): `series_key` ->
// `run_id` escolhido explicitamente por um humano, com data e motivo.
// Imutável -- substituir uma baseline é acrescentar entrada nova, nunca
// reescrever ou apagar a anterior (o arquivo é o histórico das próprias
// decisões de baseline, não só um mapa "atual").

#include <filesystem>
#include <string>
#include <vector>

namespace modb::loadtest {

struct BaselineEntry {
    std::string series_key;
    std::string run_id;
    std::string case_id;
    std::string marked_at;   // timestamp UTC de quando foi marcada (não do run_id)
    std::string reason;
};

struct BaselineLoadResult {
    bool ok{false};
    std::vector<BaselineEntry> entries;   // vazio + ok=true quando o arquivo não existe ainda
    std::string error;
};

[[nodiscard]] BaselineLoadResult load_baselines(const std::filesystem::path& path);

// Acrescenta `entry` ao arquivo (cria se não existir). NUNCA remove ou
// modifica entradas já gravadas -- mesmo marcar a "mesma" baseline de novo
// vira uma linha nova, com timestamp e motivo próprios.
[[nodiscard]] bool append_baseline(const std::filesystem::path& path, const BaselineEntry& entry);

[[nodiscard]] bool is_baseline_run(const std::vector<BaselineEntry>& entries,
                                   const std::string& run_id);

} // namespace modb::loadtest
