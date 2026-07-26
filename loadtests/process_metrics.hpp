#pragma once

// Métricas do processo atual (docs/PLANO_TESTES_DE_CARGA.md §8). O projeto não
// tinha nenhum precedente de leitura de RSS -- implementado aqui por
// plataforma, nunca falha a campanha (devolve 0 se indisponível).

#include <chrono>
#include <cstdint>

namespace modb::loadtest {

// Working set (Windows) / VmHWM (Linux) de pico do processo atual, em bytes.
// 0 se não foi possível obter.
//
// ATENÇÃO: este valor é monotônico durante toda a vida do processo. Uma
// campanha com muitas fases vê o MESMO número em todas elas depois da primeira
// fase grande -- foi exatamente esse o defeito M2 registrado em
// docs-process/PLANO_PROFILING.md §3. Para RSS por fase use `RssTracker`.
[[nodiscard]] std::uint64_t peak_rss_bytes();

// Working set (Windows) / VmRSS (Linux) *neste instante*, em bytes.
// 0 se não foi possível obter.
[[nodiscard]] std::uint64_t current_rss_bytes();

// Marca de água alta do RSS dentro de um escopo (uma fase). Não há como zerar
// o pico do processo no Windows (não existe API), então o pico por fase é
// rastreado em software: amostra-se o RSS corrente e guarda-se o máximo.
//
// `sample()` é barato o suficiente para ser chamado por operação: a consulta ao
// SO acontece no máximo uma vez por `query_interval`, para a instrumentação não
// distorcer a própria medição.
class RssTracker {
public:
    // Amostra na construção: o piso é o RSS no início da fase.
    RssTracker();

    void sample();

    // Amostra agora (sem throttle) e devolve o máximo observado no escopo.
    [[nodiscard]] std::uint64_t peak();

private:
    static constexpr std::chrono::milliseconds query_interval{100};

    std::uint64_t peak_{0};
    std::chrono::steady_clock::time_point last_query_{};
};

} // namespace modb::loadtest
