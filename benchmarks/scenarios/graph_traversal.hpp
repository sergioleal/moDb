#pragma once

#include "scenarios/object_store_lifecycle.hpp"

#include <cstdint>
#include <string>

namespace modb::bench {

struct GraphTraversalParams {
    std::string scenario_id{"graph.traversal"};
    std::uint64_t seed{0};
    // Largura (filhos por nó interno) e profundidade da árvore gerada.
    std::uint32_t branching{3};
    std::uint32_t depth{4};
    std::string cache_state{"warm"}; // warm | cold
    std::string work_dir;
};

// Gera uma árvore, reabre (se cold), e mede BFS completo + pico do visited-set.
[[nodiscard]] SampleResult run_graph_traversal(const GraphTraversalParams& params);

} // namespace modb::bench
