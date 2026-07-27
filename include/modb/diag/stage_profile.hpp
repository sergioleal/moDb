#pragma once

// Atribuição de tempo por estágio do caminho quente (docs-process/
// PLANO_PROFILING.md, Etapa 1).
//
// Por que instrumentação in-process e não um profiler amostral: nesta toolchain
// não existe profiler com símbolos para o binário Windows (MinGW emite DWARF em
// PE; VTune e WPA querem PDB). E mesmo onde existe, um profiler amostral de CPU
// não enxerga espera de I/O nem de `fsync` -- que é justamente metade da
// pergunta em aberto sobre o caminho de escrita.
//
// Custo quando desligado: zero. `ScopedStage` vira uma classe vazia e as
// funções viram no-ops inline, sem tocar em nenhum contador. O motor é compilado
// sem instrumentação por padrão; o preset `stage-profile` liga.
//
// Custo quando ligado: uma leitura de relógio monotônico (QPC no Windows,
// ~20-25 ns) e um fetch_add relaxed por escopo. Os estágios instrumentados são
// escolhidos para ficarem na casa do microssegundo, então a distorção fica na
// casa de 1-2%. Estágios muito mais finos que isso não devem ser instrumentados
// aqui -- meça-os por contagem, não por tempo.

#include <array>
#include <cstdint>
#include <string_view>

#if defined(MODB_ENABLE_STAGE_PROFILING)
#include <chrono>
#endif

namespace modb::diag {

// Taxonomia fixa. Nomes estáveis: eles saem no JSONL e entram em série
// histórica, então renomear um estágio é como renomear um `scenario_id`.
enum class Stage : std::size_t {
    // Laço de candidatas de TableHeap::insert -- o suspeito principal do termo
    // proporcional à contagem de páginas. `units` conta ITERAÇÕES do laço.
    heap_candidate_scan,
    // Tentativa de inserir numa página candidata produzida pelo laço acima.
    // `calls` é o que interessa: se ficar em zero enquanto heap_candidate_scan
    // itera milhares de vezes por operação, a varredura é trabalho puro sem
    // resultado. `units` conta as candidatas que estavam cheias na prática.
    heap_candidate_try,
    // Escrita de uma página do heap no PageFile. `units` conta bytes.
    heap_page_write,
    // TableHeap::persist_root -- a escrita extra da página raiz por operação.
    persist_root,
    // Wal::append_page_image e afins. `units` conta bytes de WAL.
    wal_append,
    // Wal::sync -- o `fsync` de verdade. `units` não é usado.
    wal_sync,
    // Codificação do objeto (ObjectCodec). `units` conta bytes lógicos.
    object_encode,

    count_
};

inline constexpr std::size_t stage_count = static_cast<std::size_t>(Stage::count_);

[[nodiscard]] std::string_view stage_name(Stage stage) noexcept;

struct StageTotals {
    std::uint64_t elapsed_ns{0};
    std::uint64_t calls{0};
    // Contador livre, com significado próprio por estágio (ver o enum). Sempre
    // 0 quando o estágio não define um -- nunca um número inventado.
    std::uint64_t units{0};
};

using StageSnapshot = std::array<StageTotals, stage_count>;

#if defined(MODB_ENABLE_STAGE_PROFILING)

inline constexpr bool stage_profiling_enabled = true;

// Acumuladores globais em atômicos relaxed, não thread_local: o motor tem um
// único escritor de transação (ADR-011), mas `mixed_oltp` emite operações de
// várias threads, e somar thread_locals exigiria um registro de threads vivas.
// Relaxed basta -- ninguém lê os contadores para sincronizar nada, só para
// somar no fim da fase.
void stage_record(Stage stage, std::uint64_t elapsed_ns, std::uint64_t units) noexcept;
void stage_add_units(Stage stage, std::uint64_t units) noexcept;
void stage_reset() noexcept;
[[nodiscard]] StageSnapshot stage_snapshot() noexcept;

class ScopedStage {
public:
    explicit ScopedStage(Stage stage) noexcept
        : stage_{stage}, start_{std::chrono::steady_clock::now()} {}

    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;

    // `units` é acumulado junto ao fechar o escopo; quem mede iterações ou
    // bytes só sabe o total no fim.
    void add_units(std::uint64_t units) noexcept { units_ += units; }

    ~ScopedStage() {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        stage_record(stage_, static_cast<std::uint64_t>(
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                                     .count()),
                     units_);
    }

private:
    Stage stage_;
    std::chrono::steady_clock::time_point start_;
    std::uint64_t units_{0};
};

#else

inline constexpr bool stage_profiling_enabled = false;

inline void stage_record(Stage, std::uint64_t, std::uint64_t) noexcept {}
inline void stage_add_units(Stage, std::uint64_t) noexcept {}
inline void stage_reset() noexcept {}
[[nodiscard]] inline StageSnapshot stage_snapshot() noexcept { return {}; }

class ScopedStage {
public:
    explicit ScopedStage(Stage) noexcept {}
    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;
    void add_units(std::uint64_t) noexcept {}
};

#endif

} // namespace modb::diag
