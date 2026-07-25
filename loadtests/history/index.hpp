#pragma once

// Indexação (docs/PLANO_TESTES_DE_CARGA.md §13.5): extrai rollups de uma
// campanha e faz append idempotente em load-history/series.jsonl.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace modb::loadtest {

struct IndexResult {
    bool ok{false};
    std::uint64_t appended{};
    std::uint64_t skipped_duplicate{};
    std::uint64_t rejected{};
    std::vector<std::string> rejection_reasons;
    std::string error;   // erro fatal (não consegue ler/escrever arquivo)
};

// Idempotente: reindexar o mesmo `campaign_path` sobre o mesmo
// `history_path` não duplica (dedup por run_id+case_id+repeat_index).
// Rollup sem commit/series_key/environment/host_class/build_type/seed/status
// é rejeitado, nunca aceito em silêncio (§13.3).
[[nodiscard]] IndexResult index_campaign(const std::filesystem::path& campaign_path,
                                         const std::filesystem::path& history_path,
                                         const std::filesystem::path& environments_file);

} // namespace modb::loadtest
