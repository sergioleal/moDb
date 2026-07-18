#pragma once

// Planner determinístico da Fase 7E: escolhe Index Scan ou Scan + Predicate,
// classifica a natureza do plano, decide pushdown de Limit / seleção de Top-K
// e expõe first_result_cost (objetos estimados até o 1º yield) — base do TTFR.
// Sem estatísticas de cardinalidade no MVP: regras fixas, reproduzíveis.

#include "modb/query/operators.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace modb::query {

enum class AccessMethod : std::uint8_t {
    table_scan = 0,
    index_scan = 1,
};

[[nodiscard]] constexpr const char* access_name(AccessMethod access) noexcept {
    switch (access) {
    case AccessMethod::table_scan:
        return "table_scan";
    case AccessMethod::index_scan:
        return "index_scan";
    }
    return "unknown";
}

struct QueryPlan {
    AccessMethod access{AccessMethod::table_scan};
    OperatorNature nature{OperatorNature::streaming};
    // Objetos estimados lidos até o primeiro yield. kFullInput = materializa
    // a entrada antes de ceder (sort / top_k).
    std::size_t first_result_cost{1};
    bool limit_pushed{false};
    bool uses_top_k{false};
    bool uses_sort{false};
    bool uses_distinct{false};
    // Predicado equals/between pedido e índice disponível no catálogo.
    bool index_requested{false};
    bool index_available{false};
    std::optional<std::uint16_t> index_field{};

    static constexpr std::size_t kFullInput = (std::numeric_limits<std::size_t>::max)();

    [[nodiscard]] const char* access_name() const noexcept {
        return query::access_name(access);
    }
};

// Descrição tipada da intenção da consulta (sem materializar o Snapshot).
struct QueryIntent {
    bool index_requested{false};
    bool index_available{false};
    std::optional<std::uint16_t> index_field{};
    bool has_opaque_predicate{false};
    std::size_t limit{0};
    std::size_t top_k{0};
    bool has_order{false};
    bool has_distinct{false};
};

// Regras (MVP, determinísticas):
// 1. equals/between + índice → index_scan; senão table_scan (+ filter).
// 2. top_k explícito → parcialmente bloqueante.
// 3. order_by + limit (sem distinct) → seleciona Top-K (não sort completo).
// 4. order_by sem limit → sort bloqueante.
// 5. distinct → bloqueante.
// 6. limit só em plano streaming → pushdown na fonte.
[[nodiscard]] inline QueryPlan plan_query(const QueryIntent& intent) noexcept {
    QueryPlan plan;
    plan.index_requested = intent.index_requested;
    plan.index_available = intent.index_available;
    plan.index_field = intent.index_field;

    if (intent.index_requested && intent.index_available) {
        plan.access = AccessMethod::index_scan;
    } else {
        plan.access = AccessMethod::table_scan;
    }

    const bool select_top_k =
        intent.has_order && intent.limit > 0 && intent.top_k == 0 && !intent.has_distinct;

    if (intent.top_k > 0) {
        plan.uses_top_k = true;
        plan.nature = OperatorNature::partially_blocking;
        plan.first_result_cost = QueryPlan::kFullInput;
        plan.limit_pushed = false;
    } else if (select_top_k) {
        plan.uses_top_k = true;
        plan.nature = OperatorNature::partially_blocking;
        plan.first_result_cost = QueryPlan::kFullInput;
        plan.limit_pushed = false;
    } else if (intent.has_order) {
        plan.uses_sort = true;
        plan.nature = OperatorNature::blocking;
        plan.first_result_cost = QueryPlan::kFullInput;
        plan.limit_pushed = false;
    } else if (intent.has_distinct) {
        plan.uses_distinct = true;
        plan.nature = OperatorNature::blocking;
        // Distinct pode ceder o primeiro item assim que a chave aparece.
        plan.first_result_cost = 1;
        plan.limit_pushed = intent.limit > 0;
    } else {
        plan.nature = OperatorNature::streaming;
        plan.first_result_cost = 1;
        plan.limit_pushed = intent.limit > 0;
    }

    (void)intent.has_opaque_predicate;
    return plan;
}

} // namespace modb::query
