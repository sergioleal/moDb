#pragma once

// Matriz de dimensões do teste de carga (docs/PLANO_TESTES_DE_CARGA.md §4-§6).
// Subfase A: catálogo de ids, identidade de caso e expansão de seletores.
// Não executa nada — só decide QUAIS casos existem.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modb::loadtest {

// ---- D1: escala ------------------------------------------------------

struct ScaleInfo {
    std::string id;
    std::uint64_t objects{};
};

// Catálogo estável, na ordem do §4.1 (1k é smoke, fora da faixa oficial).
[[nodiscard]] const std::vector<ScaleInfo>& known_scales();
[[nodiscard]] std::optional<std::uint64_t> objects_for_scale(std::string_view scale_id);
[[nodiscard]] bool is_known_scale(std::string_view scale_id);

// ---- D2: workload ------------------------------------------------------

// Catálogo completo (§4.2 escada básica + §4.2.1 adicionais). Nem todo id
// tem dispatch de execução ainda — ver `is_workload_implemented`. O catálogo
// existe para que `list-cases`/seletores conheçam todo id válido desde a
// Subfase A, mesmo que `run` só consiga executar `create_only` por ora.
[[nodiscard]] const std::vector<std::string>& known_workloads();
[[nodiscard]] bool is_known_workload(std::string_view workload_id);
[[nodiscard]] bool is_workload_implemented(std::string_view workload_id);

// ---- D3: alvo de execução ------------------------------------------------------

[[nodiscard]] const std::vector<std::string>& known_targets();
[[nodiscard]] bool is_known_target(std::string_view target_id);
[[nodiscard]] bool is_target_implemented(std::string_view target_id);

// ---- Caso -----------------------------------------------------------

// Um ponto totalmente resolvido da matriz. `environment` (D4) é atribuído
// separadamente do perfil — não participa de `case_id()` (§4.4/§5).
struct Case {
    std::string workload;
    std::string target;
    std::string scale;
    std::uint64_t objects{};
    std::string environment;

    // Dimensões secundárias (§4.5), sempre com o padrão declarado no plano.
    std::string payload{"normal"};
    std::uint64_t batch{1000};
    std::uint64_t concurrency{1};
    std::uint64_t readers{0};
    std::string durability{"sync_real"};
    std::string cache{"warm"};
    std::string primary_storage{"full"};
    std::uint64_t repeat_index{0};

    // Sufixo explícito, só usado quando o caso veio de `--case` com uma
    // variante que o parser não decompõe em dimensões concretas (preserva o
    // id exatamente como pedido). Vazio nos casos gerados por perfil.
    std::string explicit_variant;

    // "load.<workload>.<target>.<scale>[.<variante>]" (§5). O sufixo de
    // variante só aparece quando alguma dimensão secundária sai do padrão.
    [[nodiscard]] std::string case_id() const;
};

// Tenta reconstruir um Case a partir de um case_id explícito (`--case`).
// Reconhece só "load.<workload>.<target>.<scale>" e "...<scale>.<variante>";
// a variante fica opaca em `explicit_variant` (não decodifica dimensões
// secundárias a partir dela). Preenche `error` e devolve false se o
// workload/alvo/escala não forem reconhecidos ou o formato não bater.
[[nodiscard]] bool parse_case_id(std::string_view case_id, Case& out, std::string& error);

// ---- Seletores e composição (§6.1) -----------------------------------

struct MatrixSelectors {
    std::vector<std::string> scale;
    std::vector<std::string> workload;
    std::vector<std::string> target;
    std::vector<std::string> environment;   // cross-product quando > 1 valor
    std::vector<std::string> concurrency;   // idem
    std::vector<std::string> payload;       // idem
    std::vector<std::string> case_ids;      // --case; substitui perfil e demais seletores de D1-D3
    std::vector<std::string> exclude;       // substring, subtrai por último
    std::string filter;                     // substring no case_id
    std::uint64_t repeat{1};
};

struct ExpandResult {
    std::vector<Case> cases;
    std::string error;   // não vazio = falhou; `cases` fica vazio
};

// Expande `profile_cases` (o conjunto inicial de um perfil; vazio se
// `selectors.case_ids` for usado) segundo a semântica de composição de §6.1:
// 1. `--case` substitui tudo por uma lista explícita;
// 2. senão, cada seletor de dimensão (scale/workload/target) faz interseção
//    com o conjunto do perfil;
// 3. environment/concurrency/payload multiplicam o conjunto (cross-product)
//    quando mais de um valor é dado; um único valor sobrescreve o padrão;
// 4. `--repeat N` multiplica por N (`repeat_index` 0..N-1);
// 5. `--filter` restringe por substring no `case_id`;
// 6. `--exclude` subtrai por substring, por último.
// Um identificador de escala/workload/alvo desconhecido é erro imediato.
// Conjunto vazio ao final também é erro (§6.1 regra 5) — nunca "0 casos".
[[nodiscard]] ExpandResult expand_matrix(const std::vector<Case>& profile_cases,
                                         const MatrixSelectors& selectors);

} // namespace modb::loadtest
