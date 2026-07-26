#pragma once

// Retenção de brutos (docs/PLANO_TESTES_DE_CARGA.md §13.8). Rollups em
// `series.jsonl` NUNCA são apagados ou reescritos aqui -- só os arquivos de
// campanha brutos (`raw_file`) que já não precisam ficar no disco. O rollup
// do bruto removido permanece, com o hash, para que a ausência seja
// detectável (§13.8).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace modb::loadtest {

struct PruneOptions {
    std::filesystem::path history_path;
    std::filesystem::path raw_dir;         // diretório onde os `raw_file` vivem
    std::filesystem::path baselines_path;   // opcional; ausente = nenhuma baseline protege nada
    std::uint64_t keep{10};                // manter os N mais recentes por série (§13.8 padrão)
    bool confirm{false};                    // sem confirm = só lista o que seria removido
};

struct PruneCandidate {
    std::string series_key;
    std::string run_id;
    std::string raw_file;
    std::string reason;   // por que é candidato (ou por que foi PRESERVADO, ver `kept`)
    bool kept{false};
};

struct PruneResult {
    bool ok{false};
    std::string error;
    std::vector<PruneCandidate> candidates;   // todo bruto avaliado, mantido ou não (kept diz qual)
    std::vector<std::string> deleted;         // só preenchido quando `confirm=true`
};

[[nodiscard]] PruneResult prune_raw_files(const PruneOptions& options);

} // namespace modb::loadtest
