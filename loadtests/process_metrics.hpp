#pragma once

// Métricas do processo atual (docs/PLANO_TESTES_DE_CARGA.md §8). O projeto não
// tinha nenhum precedente de leitura de RSS -- implementado aqui por
// plataforma, nunca falha a campanha (devolve 0 se indisponível).

#include <cstdint>

namespace modb::loadtest {

// Working set (Windows) / VmHWM (Linux) de pico do processo atual, em bytes.
// 0 se não foi possível obter.
[[nodiscard]] std::uint64_t peak_rss_bytes();

} // namespace modb::loadtest
