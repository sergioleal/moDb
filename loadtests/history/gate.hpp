#pragma once

// Gate de regressão (docs/PLANO_TESTES_DE_CARGA.md §13.7): dois mecanismos
// distintos sobre a mesma série, porque pegam coisas distintas.
//
//   gate por execução -- candidato × mediana das últimas 5 ANTERIORES da
//   mesma série (reaproveita `compute_trend`/Subfase C sem duplicar a lógica
//   de mediana móvel: o veredito do ÚLTIMO ponto já É o gate por execução).
//   Pega regressão abrupta introduzida por um commit.
//
//   deriva lenta -- mediana das últimas 5 (incluindo o candidato) × mediana
//   das 5 que terminam 20 execuções atrás, limiar único de 15%. Pega
//   degradação de 1-2% por execução que nenhum gate pontual acusaria.

#include "history/trend.hpp"

#include <filesystem>
#include <string>

namespace modb::loadtest {

struct GateResult {
    bool ok{false};   // false = erro (métrica/arquivo/série inválidos), não veredito de regressão
    std::string error;

    // "ok" | "alert" | "fail" | "insufficient". `status != "completed"` no
    // último ponto vira "fail" direto (§13.7/§9: divergência de correção é
    // falha imediata, nunca uma comparação de limiar).
    std::string point_verdict{"insufficient"};
    double point_vs_median{};

    // "ok" | "fail" | "insufficient" -- um só limiar (15%), sem tier de alerta.
    std::string drift_verdict{"insufficient"};
    double drift_ratio{};

    // false se point_verdict=="fail" OU drift_verdict=="fail". "alert" e
    // "insufficient" nunca reprovam (§13.7: histórico insuficiente é
    // sucesso, para não bloquear CI por falta de dados).
    bool passed{true};
};

[[nodiscard]] GateResult compute_gate(const std::filesystem::path& history_path,
                                      const std::string& case_id, const std::string& metric_id,
                                      const std::string& phase);

[[nodiscard]] std::string render_gate(const GateResult& result, const std::string& case_id,
                                      const std::string& metric_id);

} // namespace modb::loadtest
