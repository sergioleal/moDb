#pragma once

// Extração campanha (§12) -> rollup (§13.3), docs/PLANO_TESTES_DE_CARGA.md.
// Um objeto de campanha bruta produz um rollup por caso; casos com dispatch
// ainda não implementado (case_error citando isso) não viram rollup -- não
// são uma medição, são uma lacuna já declarada em outro lugar.

#include <filesystem>
#include <string>
#include <vector>

namespace modb::loadtest {

struct RollupExtractResult {
    bool ok{false};
    std::vector<std::string> rollup_lines;   // um JSON por caso, já serializado
    std::string error;
};

// `environments_file` resolve host_class a partir do ambiente registrado do
// caso (§4.4); opcional -- sem catálogo, host_class fica vazio e é o
// indexador (não a extração) quem recusa rollup incompleto (§13.3).
[[nodiscard]] RollupExtractResult extract_rollups(const std::filesystem::path& campaign_path,
                                                  const std::filesystem::path& environments_file);

} // namespace modb::loadtest
